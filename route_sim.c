// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IP Routing Table Simulation
 * Based on Linux kernel routing implementation
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/socket.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/netfilter_ipv4.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <net/ip.h>
#include <net/protocol.h>
#include <net/route.h>
#include <net/tcp.h>
#include <net/sock.h>
#include <net/ip_fib.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/checksum.h>
#include <net/xfrm.h>
#include <net/netevent.h>
#include <net/rtnetlink.h>

/* Route Cache Parameters */
#define RT_CACHE_CAPACITY 4096
#define RT_HASH_MASK (RT_CACHE_CAPACITY - 1)
#define RT_GC_TIMEOUT (300*HZ)
#define RT_CACHE_MIN_DST 256
#define RT_CACHE_MAX_SIZE (256*1024)

/* Route Flags */
#define RTF_UP          0x0001  /* Route usable */
#define RTF_GATEWAY     0x0002  /* Destination is a gateway */
#define RTF_HOST        0x0004  /* Host entry (net otherwise) */
#define RTF_REINSTATE   0x0008  /* Reinstate route after timeout */
#define RTF_DYNAMIC     0x0010  /* Created dynamically (by redirect) */
#define RTF_MODIFIED    0x0020  /* Modified dynamically (by redirect) */
#define RTF_MTU         0x0040  /* Specific MTU for this route */
#define RTF_MSS         0x0080  /* Specific MSS for this route */
#define RTF_WINDOW      0x0100  /* Specific window for this route */
#define RTF_IRTT        0x0200  /* Initial RTT for this route */
#define RTF_REJECT      0x0400  /* Reject route */

/* Route Cache Entry */
struct rt_cache_entry {
    struct rt_cache_entry *next;
    struct rt_key {
        __be32 dst;
        __be32 src;
        int iif;
        int oif;
        __u8 tos;
        __u8 scope;
    } key;
    struct {
        __be32 gw;
        int oif;
        unsigned flags;
        unsigned mtu;
        unsigned window;
        unsigned rtt;
        struct neighbour *neigh;
        atomic_t refcnt;
        unsigned long expires;
    } info;
    unsigned long lastuse;
    union {
        struct rtable *rt_next;
        struct rt_cache_entry *rt_cache_next;
    };
};

/* Route Cache Hash Table */
static struct rt_cache_entry *rt_hash_table[RT_CACHE_CAPACITY];
static DEFINE_SPINLOCK(rt_hash_lock);

/* Route Cache Statistics */
struct rt_cache_stat {
    unsigned long hits;
    unsigned long misses;
    unsigned long expired;
    unsigned long in_slow_tot;
    unsigned long out_slow_tot;
    unsigned long gc_total;
    unsigned long gc_ignored;
    unsigned long gc_goal_miss;
    unsigned long gc_dst_overflow;
};

static struct rt_cache_stat rt_cache_stat;

/* Route Cache Functions */

static inline unsigned int rt_hash(__be32 dst, __be32 src, int iif, __u8 tos)
{
    unsigned int h = (__force unsigned int)dst;
    h = jhash_3words(h, (__force unsigned int)src, iif, tos);
    return h & RT_HASH_MASK;
}

static struct rt_cache_entry *rt_cache_alloc(void)
{
    struct rt_cache_entry *rce;
    
    rce = kmalloc(sizeof(*rce), GFP_ATOMIC);
    if (rce) {
        memset(rce, 0, sizeof(*rce));
        atomic_set(&rce->info.refcnt, 1);
    }
    return rce;
}

static void rt_cache_free(struct rt_cache_entry *rce)
{
    if (atomic_dec_and_test(&rce->info.refcnt)) {
        if (rce->info.neigh)
            neigh_release(rce->info.neigh);
        kfree(rce);
    }
}

static int rt_cache_valid(const struct rt_cache_entry *rce)
{
    if (rce->info.expires && time_after(jiffies, rce->info.expires))
        return 0;
    return 1;
}

/* Route Cache Lookup */
static struct rt_cache_entry *rt_cache_lookup(__be32 dst, __be32 src,
                                            int iif, __u8 tos)
{
    unsigned int hash = rt_hash(dst, src, iif, tos);
    struct rt_cache_entry *rce;
    
    rcu_read_lock();
    for (rce = rcu_dereference(rt_hash_table[hash]); rce;
         rce = rcu_dereference(rce->next)) {
        if (rce->key.dst == dst &&
            rce->key.src == src &&
            rce->key.iif == iif &&
            rce->key.tos == tos) {
            if (rt_cache_valid(rce)) {
                atomic_inc(&rce->info.refcnt);
                rce->lastuse = jiffies;
                rt_cache_stat.hits++;
                rcu_read_unlock();
                return rce;
            }
            break;
        }
    }
    rcu_read_unlock();
    rt_cache_stat.misses++;
    return NULL;
}

/* Route Cache Insert */
static void rt_cache_insert(struct rt_cache_entry *rce)
{
    unsigned int hash = rt_hash(rce->key.dst, rce->key.src,
                              rce->key.iif, rce->key.tos);
    struct rt_cache_entry **rp;
    
    spin_lock_bh(&rt_hash_lock);
    rp = &rt_hash_table[hash];
    rce->next = *rp;
    rcu_assign_pointer(*rp, rce);
    spin_unlock_bh(&rt_hash_lock);
}

/* Route Cache Delete */
static void rt_cache_delete(struct rt_cache_entry *rce)
{
    unsigned int hash = rt_hash(rce->key.dst, rce->key.src,
                              rce->key.iif, rce->key.tos);
    struct rt_cache_entry **rp;
    
    spin_lock_bh(&rt_hash_lock);
    for (rp = &rt_hash_table[hash]; *rp; rp = &(*rp)->next) {
        if (*rp == rce) {
            rcu_assign_pointer(*rp, rce->next);
            spin_unlock_bh(&rt_hash_lock);
            rt_cache_free(rce);
            return;
        }
    }
    spin_unlock_bh(&rt_hash_lock);
}

/* Route Cache Garbage Collection */
static void rt_cache_gc(unsigned long dummy)
{
    int i;
    struct rt_cache_entry *rce, **rp;
    unsigned long now = jiffies;
    unsigned long next_gc = now + RT_GC_TIMEOUT;
    
    rt_cache_stat.gc_total++;
    
    spin_lock_bh(&rt_hash_lock);
    for (i = 0; i < RT_CACHE_CAPACITY; i++) {
        rp = &rt_hash_table[i];
        while ((rce = *rp) != NULL) {
            if (!rt_cache_valid(rce) ||
                time_after(now, rce->lastuse + RT_GC_TIMEOUT)) {
                *rp = rce->next;
                rt_cache_free(rce);
                rt_cache_stat.expired++;
            } else {
                rp = &rce->next;
                if (time_before(rce->info.expires, next_gc))
                    next_gc = rce->info.expires;
            }
        }
    }
    spin_unlock_bh(&rt_hash_lock);
}

/* Route Input Processing */
int ip_route_input_slow(struct sk_buff *skb, __be32 daddr, __be32 saddr,
                       __u8 tos, struct net_device *dev)
{
    struct rt_cache_entry *rce;
    struct fib_result res;
    int err;
    
    /* Check for martian addresses */
    if (ipv4_is_multicast(saddr) || ipv4_is_lbcast(saddr) ||
        ipv4_is_loopback(saddr)) {
        goto martian_source;
    }
    
    /* Route lookup in FIB */
    err = fib_lookup(dev_net(dev), &res);
    if (err) {
        if (err == -EINVAL)
            goto martian_destination;
        goto no_route;
    }
    
    /* Create route cache entry */
    rce = rt_cache_alloc();
    if (!rce)
        goto no_route;
    
    /* Fill route cache entry */
    rce->key.dst = daddr;
    rce->key.src = saddr;
    rce->key.iif = dev->ifindex;
    rce->key.tos = tos;
    rce->key.scope = res.scope;
    
    rce->info.gw = res.gw;
    rce->info.oif = res.oif;
    rce->info.flags = res.flags;
    rce->info.mtu = res.mtu;
    rce->info.window = res.window;
    rce->info.rtt = res.rtt;
    
    rce->lastuse = jiffies;
    rce->info.expires = jiffies + RT_GC_TIMEOUT;
    
    /* Insert into route cache */
    rt_cache_insert(rce);
    
    return 0;

martian_destination:
    rt_cache_stat.in_slow_tot++;
    return -EINVAL;

martian_source:
    rt_cache_stat.in_slow_tot++;
    return -EINVAL;

no_route:
    rt_cache_stat.in_slow_tot++;
    return -EHOSTUNREACH;
}

/* Route Output Processing */
struct rt_cache_entry *ip_route_output_slow(__be32 daddr, __be32 saddr,
                                          __u8 tos, int oif)
{
    struct rt_cache_entry *rce;
    struct fib_result res;
    int err;
    
    /* Route lookup in FIB */
    err = fib_lookup(dev_net(init_net.loopback_dev), &res);
    if (err)
        goto no_route;
    
    /* Create route cache entry */
    rce = rt_cache_alloc();
    if (!rce)
        goto no_route;
    
    /* Fill route cache entry */
    rce->key.dst = daddr;
    rce->key.src = saddr;
    rce->key.iif = 0;
    rce->key.oif = oif;
    rce->key.tos = tos;
    rce->key.scope = res.scope;
    
    rce->info.gw = res.gw;
    rce->info.oif = res.oif;
    rce->info.flags = res.flags;
    rce->info.mtu = res.mtu;
    rce->info.window = res.window;
    rce->info.rtt = res.rtt;
    
    rce->lastuse = jiffies;
    rce->info.expires = jiffies + RT_GC_TIMEOUT;
    
    /* Insert into route cache */
    rt_cache_insert(rce);
    
    return rce;

no_route:
    rt_cache_stat.out_slow_tot++;
    return NULL;
}

/* Route Cache Initialization */
void __init ip_rt_init(void)
{
    int i;
    
    /* Initialize hash table */
    for (i = 0; i < RT_CACHE_CAPACITY; i++)
        rt_hash_table[i] = NULL;
    
    /* Initialize statistics */
    memset(&rt_cache_stat, 0, sizeof(rt_cache_stat));
    
    /* Initialize garbage collection timer */
    init_timer(&rt_gc_timer);
    rt_gc_timer.function = rt_cache_gc;
    rt_gc_timer.expires = jiffies + RT_GC_TIMEOUT;
    add_timer(&rt_gc_timer);
}

/* Route Cache Cleanup */
void __exit ip_rt_cleanup(void)
{
    int i;
    struct rt_cache_entry *rce, *next;
    
    /* Delete timer */
    del_timer(&rt_gc_timer);
    
    /* Free all route cache entries */
    for (i = 0; i < RT_CACHE_CAPACITY; i++) {
        for (rce = rt_hash_table[i]; rce; rce = next) {
            next = rce->next;
            rt_cache_free(rce);
        }
        rt_hash_table[i] = NULL;
    }
}

/* ICMP Redirect Processing */
void ip_rt_send_redirect(struct sk_buff *skb)
{
    struct iphdr *iph = ip_hdr(skb);
    struct rt_cache_entry *rce;
    
    rce = rt_cache_lookup(iph->daddr, iph->saddr,
                         skb->dev->ifindex, iph->tos);
    if (!rce)
        return;
    
    if (!(rce->info.flags & RTF_GATEWAY))
        return;
    
    icmp_send(skb, ICMP_REDIRECT, ICMP_REDIR_HOST, rce->info.gw);
    rt_cache_free(rce);
}

/* Route MTU Update */
void ip_rt_update_pmtu(struct dst_entry *dst, struct sock *sk,
                      struct sk_buff *skb, u32 mtu)
{
    struct rt_cache_entry *rce;
    
    rce = container_of(dst, struct rt_cache_entry, dst);
    if (!rce)
        return;
    
    if (mtu < 68 || mtu >= IP_MAX_MTU)
        return;
    
    if (rce->info.mtu == mtu)
        return;
    
    rce->info.mtu = mtu;
    rce->info.expires = jiffies + 10*HZ;
}

/* Route Error Processing */
void ip_rt_send_error(struct sk_buff *skb)
{
    struct iphdr *iph = ip_hdr(skb);
    struct rt_cache_entry *rce;
    
    rce = rt_cache_lookup(iph->daddr, iph->saddr,
                         skb->dev->ifindex, iph->tos);
    if (!rce)
        return;
    
    if (time_before(jiffies, rce->info.expires))
        rce->info.expires = jiffies + 10*HZ;
    
    rt_cache_free(rce);
}

/* Route Cache Statistics */
void rt_cache_get_stats(struct rt_cache_stat *stats)
{
    memcpy(stats, &rt_cache_stat, sizeof(*stats));
}

/* Route Cache Debug Information */
void rt_cache_debug(void)
{
    int i;
    struct rt_cache_entry *rce;
    
    printk(KERN_INFO "Route Cache Debug Information:\n");
    printk(KERN_INFO "--------------------------------\n");
    printk(KERN_INFO "Cache Statistics:\n");
    printk(KERN_INFO "  Hits: %lu\n", rt_cache_stat.hits);
    printk(KERN_INFO "  Misses: %lu\n", rt_cache_stat.misses);
    printk(KERN_INFO "  Expired: %lu\n", rt_cache_stat.expired);
    printk(KERN_INFO "  GC Total: %lu\n", rt_cache_stat.gc_total);
    
    printk(KERN_INFO "\nCache Entries:\n");
    for (i = 0; i < RT_CACHE_CAPACITY; i++) {
        for (rce = rt_hash_table[i]; rce; rce = rce->next) {
            printk(KERN_INFO "  [%d] DST=%pI4 SRC=%pI4 TOS=%u IIF=%d\n",
                   i, &rce->key.dst, &rce->key.src,
                   rce->key.tos, rce->key.iif);
            printk(KERN_INFO "      GW=%pI4 OIF=%d FLAGS=0x%x MTU=%u\n",
                   &rce->info.gw, rce->info.oif,
                   rce->info.flags, rce->info.mtu);
            printk(KERN_INFO "      EXPIRES=%lu LASTUSE=%lu\n",
                   rce->info.expires, rce->lastuse);
        }
    }
}

module_init(ip_rt_init);
module_exit(ip_rt_cleanup);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IP Routing Table Simulation");
MODULE_AUTHOR("Your Name");
