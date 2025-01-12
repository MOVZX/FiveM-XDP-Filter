#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <bpf/bpf_helpers.h>

#define htons(x) ((__be16)___constant_swab16((x)))
#define htonl(x) ((__be32)___constant_swab32((x)))

#define FIVEM_SERVER_IP     0x7F000001      // 127.0.0.1
#define FIVEM_SERVER_PORT   30120           // Replace with the port number of your FiveM/RedM server
#define MUMBLE_SERVER_PORT  30121           // Replace with the port number of your Mumble/PMA server
#define HTTP_SERVER_PORT    80              // Replace with the port number of your HTTP server (Nginx)
#define HTTPS_SERVER_PORT   443             // Replace with the port number of your HTTPS server (Nginx)
#define RATE_LIMIT          13000           // Maximum number of packets allowed per second

struct
{
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} rate_limit_map SEC(".maps");

struct
{
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2); // 0: dropped, 1: passed
    __type(key, __u32);
    __type(value, __u64);
} packet_count_map SEC(".maps");

SEC("prog")

int fivem_xdp(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;
    __u32 rate_limit_key = 0;
    __u64 *rate_limit_value;
    __u32 key_dropped = 0;
    __u32 key_passed = 1;
    __u64 *count_dropped, *count_passed;
    count_dropped = bpf_map_lookup_elem(&packet_count_map, &key_dropped);
    count_passed = bpf_map_lookup_elem(&packet_count_map, &key_passed);

    if (!count_dropped || !count_passed)
        return XDP_ABORTED;

    // Parse Ethernet header
    struct ethhdr *eth = data;

    if ((void *)(eth + 1) > data_end)
        return XDP_ABORTED;

    // Check that the packet is IPv4
    if (eth->h_proto != htons(ETH_P_IP))
        return XDP_PASS; // Allow non-IPv4 packets

    // Parse IP Header and validate its length
    struct iphdr *ip = data + sizeof(struct ethhdr);

    if ((void *)(ip + 1) > data_end || ip->ihl < 5)
        return XDP_ABORTED;

    if (ip->protocol == IPPROTO_UDP)
    {
        // Parse UDP header
        struct udphdr *udp = data + sizeof(struct ethhdr) + (ip->ihl * 4);

        if ((void *)(udp + 1) > data_end)
            return XDP_ABORTED;

        // Check if packet is destined for the FiveM/Mumble/PMA server IP and port
        if
        (
            ip->daddr != htonl(FIVEM_SERVER_IP) ||
            (
                udp->dest != htons(FIVEM_SERVER_PORT) && // FiveM/RedM
                udp->dest != htons(MUMBLE_SERVER_PORT) // Mumble/PMA
            )
        )
            return XDP_PASS; // Allow other UDP traffic
    }

    else if (ip->protocol == IPPROTO_TCP)
    {
        // Parse TCP header
        struct tcphdr *tcp = data + sizeof(struct ethhdr) + (ip->ihl * 4);

        if ((void *)(tcp + 1) > data_end)
            return XDP_ABORTED;

        // Drop SYN Flood
        if (tcp->syn == 1 && tcp->ack == 0)
            return XDP_DROP;

        // Check if packet is destined for the FiveM/Mumble/PMA/Nginx server IP and port
        if
        (
            ip->daddr != htonl(FIVEM_SERVER_IP) ||
            (
                tcp->dest != htons(FIVEM_SERVER_PORT) && // FiveM/RedM
                tcp->dest != htons(MUMBLE_SERVER_PORT) && // Mumble/PMA
                tcp->dest != htons(HTTP_SERVER_PORT) && // Nginx HTTP
                tcp->dest != htons(HTTPS_SERVER_PORT) // Nginx HTTPS
            )
        )
            return XDP_PASS; // Allow other TCP traffic
    }

    // Check the rate limit map
    rate_limit_value = bpf_map_lookup_elem(&rate_limit_map, &rate_limit_key);

    if (!rate_limit_value)
        return XDP_PASS; // Allow packets if map lookup fails

    // Get the current timestamp
    __u64 now = bpf_ktime_get_ns();
    __u64 last = *rate_limit_value;

    // Rate limiting logic
    if (now - last < (1000000000 / RATE_LIMIT))
    {
        __sync_fetch_and_add(count_dropped, 1);

        return XDP_DROP; // Drop packet if rate limit exceeded
    }

    // Update the rate limit map with the current timestamp
    bpf_map_update_elem(&rate_limit_map, &rate_limit_key, &now, BPF_ANY);

    __sync_fetch_and_add(count_passed, 1);
    return XDP_PASS; // Pass the packet if within rate limit
}

char _license[] SEC("license") = "MIT";
