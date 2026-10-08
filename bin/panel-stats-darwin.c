/*
 * macOS sampler for panel-stats. macOS has no /proc or /sys, so this prints
 * the same raw readings the Linux branch of panel-stats collects, on one line:
 *
 *   tot idle up memtotal_kb memavail_kb iface rx_bytes tx_bytes temp_c
 *
 * tot/idle   CPU ticks summed over all cores (HOST_CPU_LOAD_INFO)
 * up         monotonic seconds, only used for time deltas
 * memavail   total minus what Activity Monitor calls "Memory Used"
 *            (app memory + wired + compressed)
 * iface      interface of the default IPv4 route, "-" when there is none
 * rx/tx      64-bit byte counters of that interface (ifmib)
 * temp       hottest "PMU tdie" sensor (Apple Silicon die temperature)
 *            read through the IOHIDEventSystem, 0 when unavailable
 *
 * Build: cc -O2 -o panel-stats-darwin panel-stats-darwin.c \
 *            -framework IOKit -framework CoreFoundation
 * panel-stats rebuilds it on its own when the source is newer.
 */
#include <sys/types.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <time.h>

/* private IOKit HID event API, the same one powermetrics-free tools use */
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef struct __IOHIDEvent *IOHIDEventRef;
IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef);
int IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef, CFDictionaryRef);
CFArrayRef IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef);
CFTypeRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef, CFStringRef);
IOHIDEventRef IOHIDServiceClientCopyEvent(IOHIDServiceClientRef, int64_t, int32_t, int64_t);
double IOHIDEventGetFloatValue(IOHIDEventRef, int32_t);

#define HID_EVENT_TEMPERATURE 15
#define HID_FIELD_BASE(type) ((type) << 16)

static void cpu(unsigned long long *tot, unsigned long long *idle)
{
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
    *tot = *idle = 0;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &n) != KERN_SUCCESS)
        return;
    for (int i = 0; i < CPU_STATE_MAX; i++)
        *tot += info.cpu_ticks[i];
    *idle = info.cpu_ticks[CPU_STATE_IDLE];
}

static void mem(unsigned long long *total_kb, unsigned long long *avail_kb)
{
    uint64_t total = 0;
    size_t len = sizeof total;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;

    *total_kb = *avail_kb = 0;
    if (sysctlbyname("hw.memsize", &total, &len, NULL, 0) != 0)
        return;
    host_page_size(mach_host_self(), &page);
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &n) != KERN_SUCCESS)
        return;

    uint64_t used = ((uint64_t)vm.internal_page_count - vm.purgeable_count
                     + vm.wire_count + vm.compressor_page_count) * page;
    *total_kb = total / 1024;
    *avail_kb = used < total ? (total - used) / 1024 : 0;
}

/* interface index of the unscoped default IPv4 route, 0 when there is none */
static unsigned default_ifindex(void)
{
    int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY };
    size_t len = 0;
    unsigned idx = 0;
    char *buf;

    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || !(buf = malloc(len)))
        return 0;
    if (sysctl(mib, 6, buf, &len, NULL, 0) == 0) {
        for (char *p = buf; p < buf + len;) {
            struct rt_msghdr *rtm = (struct rt_msghdr *)p;
            struct sockaddr_in *dst = (struct sockaddr_in *)(rtm + 1);
            if (!(rtm->rtm_flags & RTF_IFSCOPE) && (rtm->rtm_addrs & RTA_DST) &&
                dst->sin_family == AF_INET && dst->sin_addr.s_addr == 0) {
                idx = rtm->rtm_index;
                break;
            }
            p += rtm->rtm_msglen;
        }
    }
    free(buf);
    return idx;
}

/* 64-bit byte counters; the if_msghdr2 route-socket copy is truncated to
 * 32 bits on current macOS, the ifmib one is not */
static void net(unsigned idx, unsigned long long *rx, unsigned long long *tx)
{
    int mib[6] = { CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA,
                   (int)idx, IFDATA_GENERAL };
    struct ifmibdata d;
    size_t len = sizeof d;

    *rx = *tx = 0;
    if (sysctl(mib, 6, &d, &len, NULL, 0) != 0)
        return;
    *rx = d.ifmd_data.ifi_ibytes;
    *tx = d.ifmd_data.ifi_obytes;
}

static CFDictionaryRef match(int page, int usage)
{
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &page);
    CFNumberRef u = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    const void *k[] = { CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage") };
    const void *v[] = { p, u };
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(p);
    CFRelease(u);
    return d;
}

static double temp(void)
{
    double best = 0;
    IOHIDEventSystemClientRef sys = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
    if (!sys)
        return 0;

    CFDictionaryRef m = match(0xff00, 5);   /* Apple vendor page, temperature */
    IOHIDEventSystemClientSetMatching(sys, m);
    CFRelease(m);

    CFArrayRef services = IOHIDEventSystemClientCopyServices(sys);
    if (services) {
        for (CFIndex i = 0; i < CFArrayGetCount(services); i++) {
            IOHIDServiceClientRef sc =
                (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
            CFTypeRef name = IOHIDServiceClientCopyProperty(sc, CFSTR("Product"));
            char buf[64] = "";
            if (name && CFGetTypeID(name) == CFStringGetTypeID())
                CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
            if (name)
                CFRelease(name);
            if (strncmp(buf, "PMU tdie", 8) != 0)
                continue;
            IOHIDEventRef ev = IOHIDServiceClientCopyEvent(sc, HID_EVENT_TEMPERATURE, 0, 0);
            if (!ev)
                continue;
            double c = IOHIDEventGetFloatValue(ev, HID_FIELD_BASE(HID_EVENT_TEMPERATURE));
            CFRelease(ev);
            if (c > best && c < 150)
                best = c;
        }
        CFRelease(services);
    }
    CFRelease(sys);
    return best;
}

int main(void)
{
    unsigned long long tot, idle, mt, ma, rx = 0, tx = 0;
    char iface[IF_NAMESIZE] = "-";
    struct timespec ts;

    cpu(&tot, &idle);
    mem(&mt, &ma);
    unsigned idx = default_ifindex();
    if (idx && if_indextoname(idx, iface))
        net(idx, &rx, &tx);
    else
        strcpy(iface, "-");
    clock_gettime(CLOCK_MONOTONIC, &ts);

    printf("%llu %llu %.2f %llu %llu %s %llu %llu %.1f\n", tot, idle,
           ts.tv_sec + ts.tv_nsec / 1e9, mt, ma, iface, rx, tx, temp());
    return 0;
}
