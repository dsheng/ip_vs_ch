/*
 * IPVS:        Consistent Hashing scheduling module
 *
 * Authors:     Bisheng Liu <mking.liu@gmail.com>
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Changes:
 *
 */

/*
 * The ch algorithm is to select server by the consistent hash of source IP
 * address. 
 *
 * Notes that there are 160 virtual nodes that maps the destinations
 * index derived from packet source IP address to the current server
 * array. If the ch scheduler is used in cache cluster, it is good to
 * combine it with cache_bypass feature. When the statically assigned
 * server is dead or overloaded, the load balancer can bypass the cache
 * server and send requests to the original server directly.
 *
 */

#define KMSG_COMPONENT "IPVS"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/list.h>
#include <linux/ip.h>
#include <net/ip_vs.h>

#include "conhash.h"

struct node_list{
    struct list_head n_list;
    struct node_s node;
};

/*
 *      IPVS CH initial data
 */
struct ip_vs_ch_bucket {
    unsigned int count;
    struct conhash_s    *conhash;
    struct list_head    node_head;
};

#define REPLICA 160


/*
 *      Get ip_vs_dest associated with supplied parameters.
 */
static inline struct ip_vs_dest *
ip_vs_ch_get(int af, struct ip_vs_ch_bucket *tbl,
         const union nf_inet_addr *addr, __be16 sport)
{
    char str[40];
    const struct node_s *node;

    sprintf(str, "%u:%u", ntohl(addr->ip),sport);
    node = conhash_lookup(tbl->conhash, str);
    return node == NULL? NULL: node->dest;

}


/*
 *      Assign all the hash buckets of the specified table with the service.
 */
static int
ip_vs_ch_assign(struct ip_vs_ch_bucket *tbl, struct ip_vs_service *svc)
{
    struct ip_vs_dest *dest;
    struct node_list *p_node;
    int weight = 0;
    char str[40];

    INIT_LIST_HEAD(&tbl->node_head);

    list_for_each_entry(dest, &svc->destinations,n_list) {
       weight = atomic_read(&dest->weight);
       if (weight > 0) {

           p_node = kmalloc(sizeof(struct node_list),GFP_ATOMIC);
           if (p_node == NULL) {
                pr_err("%s(): no memory\n", __func__);
                return -ENOMEM;
            }
           list_add_tail(&p_node->n_list,&tbl->node_head);

           atomic_inc(&dest->refcnt);
           p_node->node.dest = dest;

           sprintf(str, "%u", ntohl(dest->addr.ip));

           conhash_set_node(&p_node->node, str, weight*REPLICA);
           conhash_add_node(tbl->conhash, &p_node->node);
           
           tbl->count++;
        }
    }
    return 0;
}


/*
 *      Flush all the hash buckets of the specified table.
 */
static void ip_vs_ch_flush(struct ip_vs_ch_bucket *tbl)
{
    struct node_list *nl;

    list_for_each_entry(nl, &tbl->node_head,n_list) {
        if (nl->node.dest) {
            atomic_dec(&nl->node.dest->refcnt);
            nl->node.dest = NULL;
        }
        conhash_del_node(tbl->conhash, &nl->node);
        kfree(nl);
        tbl->count--;
    }

    list_del(&tbl->node_head);

    tbl->count = 0;
}


static int ip_vs_ch_init_svc(struct ip_vs_service *svc)
{
    struct ip_vs_ch_bucket *tbl;

    /* allocate the CH table for this service */
    tbl = kmalloc(sizeof(struct ip_vs_ch_bucket), GFP_ATOMIC);
    if (tbl == NULL) {
        pr_err("%s(): no memory\n", __func__);
        return -ENOMEM;
    }
    tbl->count = 0;
    svc->sched_data = tbl;

    IP_VS_DBG(6, "CH hash table (memory=%Zdbytes) allocated for "
          "current service\n",
          sizeof(struct ip_vs_ch_bucket));

    /* init consistent hash table */
    tbl->conhash = conhash_init(NULL);

    /* assign the hash buckets with the updated service */
    ip_vs_ch_assign(tbl, svc);

    return 0;
}


static int ip_vs_ch_done_svc(struct ip_vs_service *svc)
{
    struct ip_vs_ch_bucket *tbl = svc->sched_data;

    /* got to clean up hash buckets here */
    ip_vs_ch_flush(tbl);

    conhash_fini(tbl->conhash);

    /* release the table itself */
    kfree(svc->sched_data);
    IP_VS_DBG(6, "CH hash table (memory=%Zdbytes) released\n",
          sizeof(struct ip_vs_ch_bucket));

    return 0;
}


static int ip_vs_ch_update_svc(struct ip_vs_service *svc)
{
    struct ip_vs_ch_bucket *tbl = svc->sched_data;

    /* got to clean up hash buckets here */
    ip_vs_ch_flush(tbl);

    /* assign the hash buckets with the updated service */
    ip_vs_ch_assign(tbl, svc);

    return 0;
}

/*
 *      If the dest flags is set with IP_VS_DEST_F_OVERLOAD,
 *      consider that the server is overloaded here.
 */
static inline int is_overloaded(struct ip_vs_dest *dest)
{
    return dest->flags & IP_VS_DEST_F_OVERLOAD;
}

/*
 *      Consistent Hashing scheduling
 */
static struct ip_vs_dest *
ip_vs_ch_schedule(struct ip_vs_service *svc, const struct sk_buff *skb)
{
    struct ip_vs_dest *dest;
    struct ip_vs_ch_bucket *tbl;
    struct ip_vs_iphdr iph;
    int i = 0;
    //__be16 _ports[2], *pptr;

    ip_vs_fill_iphdr(svc->af, skb_network_header(skb), &iph);

    //pptr = skb_header_pointer(skb, iph.len, sizeof(_ports), _ports);
    //if (pptr == NULL)
    //    return NULL;

    IP_VS_DBG(6, "ip_vs_ch_schedule(): Scheduling...\n");

    tbl = (struct ip_vs_ch_bucket *)svc->sched_data;

    //dest = ip_vs_ch_get(svc->af, tbl, &iph.saddr,_ports[0]);

    for(i=0; i < tbl->count; i++){

        dest = ip_vs_ch_get(svc->af, tbl, &iph.saddr,i);

        if (!dest
            || !(dest->flags & IP_VS_DEST_F_AVAILABLE)
            || atomic_read(&dest->weight) <= 0
            || is_overloaded(dest)) {

            continue;
        }

        IP_VS_DBG_BUF(6, "CH: source IP address %s --> server %s:%d\n",
                  IP_VS_DBG_ADDR(svc->af, &iph.saddr),
                  IP_VS_DBG_ADDR(svc->af, &dest->addr),
                  ntohs(dest->port));

        return dest;

    }

    IP_VS_ERR_RL("CH: no destination available\n");

    return NULL;

}


/*
 *      IPVS CH Scheduler structure
 */
static struct ip_vs_scheduler ip_vs_ch_scheduler =
{
    .name =         "ch",
    .refcnt =         ATOMIC_INIT(0),
    .module =        THIS_MODULE,
    .n_list =          LIST_HEAD_INIT(ip_vs_ch_scheduler.n_list),
    .init_service =    ip_vs_ch_init_svc,
    .done_service =   ip_vs_ch_done_svc,
    .update_service = ip_vs_ch_update_svc,
    .schedule =       ip_vs_ch_schedule,
};


static int __init ip_vs_ch_init(void)
{
    return register_ip_vs_scheduler(&ip_vs_ch_scheduler);
}


static void __exit ip_vs_ch_cleanup(void)
{
    unregister_ip_vs_scheduler(&ip_vs_ch_scheduler);
}


module_init(ip_vs_ch_init);
module_exit(ip_vs_ch_cleanup);
MODULE_LICENSE("GPL");
