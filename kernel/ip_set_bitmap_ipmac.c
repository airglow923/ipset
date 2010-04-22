/* Copyright (C) 2000-2002 Joakim Axelsson <gozem@linux.nu>
 *                         Patrick Schaaf <bof@bof.de>
 *			   Martin Josefsson <gandalf@wlug.westbo.se>
 * Copyright (C) 2003-2010 Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* Kernel module implementing an IP set type: the bitmap:ip,mac type */

#include <linux/module.h>
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <asm/uaccess.h>
#include <asm/bitops.h>
#include <linux/spinlock.h>
#include <linux/if_ether.h>
#include <linux/netlink.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <net/netlink.h>
#include <net/pfxlen.h>

#include <linux/netfilter/ip_set.h>
#include <linux/netfilter/ip_set_timeout.h>
#include <linux/netfilter/ip_set_bitmap.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>");
MODULE_DESCRIPTION("bitmap:ip,mac type of IP sets");
MODULE_ALIAS("ip_set_bitmap:ip,mac");

enum {
	MAC_EMPTY,		/* element is not set */
	MAC_FILLED,		/* element is set with MAC */
	MAC_UNSET,		/* element is set, without MAC */
};

/* Member element without and with timeout */

struct ipmac {
	unsigned char ether[ETH_ALEN];
	unsigned char match;
};

struct ipmac_timeout {
	unsigned char ether[ETH_ALEN];
	unsigned char match;
	unsigned long timeout;
};

struct bitmap_ipmac {
	void *members;		/* the set members */
	uint32_t first_ip;	/* host byte order, included in range */
	uint32_t last_ip;	/* host byte order, included in range */
	uint32_t timeout;	/* timeout value */
	struct timer_list gc;	/* garbage collector */
	size_t elem_size;	/* size of element */
};

static inline void *
bitmap_ipmac_elem(const struct bitmap_ipmac *map, uint32_t id)
{
	return (void *)((char *)map->members + id * map->elem_size);
}

static inline bool
bitmap_timeout(const struct bitmap_ipmac *map, uint32_t id)
{
	const struct ipmac_timeout *elem = bitmap_ipmac_elem(map, id);

	return ip_set_timeout_test(elem->timeout);
}

static inline bool
bitmap_expired(const struct bitmap_ipmac *map, uint32_t id)
{
	const struct ipmac_timeout *elem = bitmap_ipmac_elem(map, id);

	return ip_set_timeout_expired(elem->timeout);
}

static inline int
bitmap_ipmac_exist(const struct ipmac *elem, bool with_timeout)
{
	const struct ipmac_timeout *e = (const struct ipmac_timeout *) elem;

	return elem->match == MAC_UNSET
	       || (elem->match == MAC_FILLED
	           && !(with_timeout && ip_set_timeout_expired(e->timeout)));
}

static inline int
bitmap_ipmac_test(const struct bitmap_ipmac *map, bool with_timeout,
		  uint32_t id, const unsigned char *ether)
{
	const struct ipmac *elem = bitmap_ipmac_elem(map, id);

	switch (elem->match) {
	case MAC_UNSET:
		/* Trigger kernel to fill out the ethernet address */
		return -EAGAIN;
	case MAC_FILLED:
		return (ether == NULL
			|| memcmp(ether, elem->ether, ETH_ALEN) == 0)
		       && (!with_timeout || bitmap_timeout(map, id));
	}
	return 0;
}

static int
bitmap_ipmac_add(struct bitmap_ipmac *map, bool with_timeout,
		 uint32_t id, const unsigned char *ether,
		 uint32_t timeout)
{
	struct ipmac *elem = bitmap_ipmac_elem(map, id);
	struct ipmac_timeout *e = (struct ipmac_timeout *) elem;

	switch (elem->match) {
	case MAC_UNSET:
		if (!ether)
			/* Already added without ethernet address */
			return -IPSET_ERR_EXIST;
		/* Fill the MAC address and activate the timer */
		memcpy(elem->ether, ether, ETH_ALEN);
		elem->match = MAC_FILLED;
		if (with_timeout) {
			if (timeout == map->timeout)
				/* Timeout was not specified, get stored one */
				timeout = e->timeout;
			e->timeout = ip_set_timeout_set(timeout);
		}
		break;
	case MAC_FILLED:
		if (!(with_timeout && bitmap_expired(map, id)))
			return -IPSET_ERR_EXIST;
		/* Fall through */
	case MAC_EMPTY:
		if (ether) {
			memcpy(elem->ether, ether, ETH_ALEN);
			elem->match = MAC_FILLED;
		} else
			elem->match = MAC_UNSET;
		if (with_timeout) {
			/* If MAC is unset yet, we store plain timeout
			 * because the timer is not activated yet
			 * and we can reuse it later when MAC is filled out,
			 * possibly by the kernel */
		 	e->timeout = ether ? ip_set_timeout_set(timeout)
					   : timeout;
		}
		break;
	}

	return 0;
}

static int
bitmap_ipmac_del(struct bitmap_ipmac *map, bool with_timeout,
		 uint32_t id)
{
	struct ipmac *elem = bitmap_ipmac_elem(map, id);

	if (elem->match == MAC_EMPTY
	    || (with_timeout && bitmap_expired(map, id)))
		return -IPSET_ERR_EXIST;

	elem->match = MAC_EMPTY;

	return 0;
}

static int
bitmap_ipmac_kadt(struct ip_set *set, const struct sk_buff *skb,
		  enum ipset_adt adt, uint8_t pf, const uint8_t *flags)
{
	struct bitmap_ipmac *map = set->data;
	uint32_t ip = ntohl(ip4addr(skb, flags));
	bool with_timeout = set->flags & IP_SET_FLAG_TIMEOUT;

	if (pf != AF_INET)
		return -EINVAL;

	if (ip < map->first_ip || ip > map->last_ip)
		return -IPSET_ERR_BITMAP_RANGE;

	if (skb_mac_header(skb) < skb->head
	    || (skb_mac_header(skb) + ETH_HLEN) > skb->data)
	    	return -EINVAL;

	ip -= map->first_ip;

	switch (adt) {
	case IPSET_TEST:
		return bitmap_ipmac_test(map, with_timeout,
					 ip, eth_hdr(skb)->h_source);
	case IPSET_ADD:
		return bitmap_ipmac_add(map, with_timeout,
					ip, eth_hdr(skb)->h_source,
					map->timeout);
	case IPSET_DEL:
		return bitmap_ipmac_del(map, with_timeout, ip);
	default:
		return -EINVAL;
	}
}

static const struct nla_policy
bitmap_ipmac_adt_policy[IPSET_ATTR_ADT_MAX + 1] __read_mostly = {
	[IPSET_ATTR_IP]		= { .type = NLA_U32 },
	[IPSET_ATTR_ETHER]	= { .type = NLA_BINARY, .len  = ETH_ALEN },
	[IPSET_ATTR_TIMEOUT]	= { .type = NLA_U32 },
};

static int
bitmap_ipmac_uadt(struct ip_set *set, struct nlattr *head, int len,
		  enum ipset_adt adt, uint32_t *lineno, uint32_t flags)
{
	struct bitmap_ipmac *map = set->data;
	struct nlattr *tb[IPSET_ATTR_ADT_MAX];
	bool eexist = flags & IPSET_FLAG_EXIST;
	bool with_timeout = set->flags & IP_SET_FLAG_TIMEOUT;
	uint32_t ip, timeout = map->timeout;
	unsigned char *ether = NULL;
	int ret = 0;

	if (nla_parse(tb, IPSET_ATTR_ADT_MAX, head, len,
		      bitmap_ipmac_adt_policy))
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_IP])
		ip = ip_set_get_h32(tb[IPSET_ATTR_IP]);
	else
		return -IPSET_ERR_PROTOCOL;

	if (ip < map->first_ip || ip > map->last_ip)
		return -IPSET_ERR_BITMAP_RANGE;

	if (tb[IPSET_ATTR_ETHER])
		ether = nla_data(tb[IPSET_ATTR_ETHER]);

	if (tb[IPSET_ATTR_TIMEOUT]) {
		if (!with_timeout)
			return -IPSET_ERR_TIMEOUT;
		timeout = ip_set_get_h32(tb[IPSET_ATTR_TIMEOUT]);
	}

	ip -= map->first_ip;

	if (adt == IPSET_TEST)
		return bitmap_ipmac_test(map, with_timeout, ip, ether);

	ret = adt == IPSET_ADD ? bitmap_ipmac_add(map, with_timeout,
						  ip, ether, timeout)
			       : bitmap_ipmac_del(map, with_timeout, ip);

	if (ret && !(ret == -IPSET_ERR_EXIST && eexist)) {
		if (tb[IPSET_ATTR_LINENO])
			*lineno = nla_get_u32(tb[IPSET_ATTR_LINENO]);
		return ret;
	}
	return ret;
}

static void
bitmap_ipmac_destroy(struct ip_set *set)
{
	struct bitmap_ipmac *map = set->data;

	/* gc might be running: del_timer_sync can't be used */
	if (set->flags & IP_SET_FLAG_TIMEOUT)
		while (!del_timer(&map->gc))
			msleep(IPSET_DESTROY_TIMER_SLEEP);
	
	ip_set_free(map->members, set->flags);
	kfree(map);
	
	set->data = NULL;
}

static void
bitmap_ipmac_flush(struct ip_set *set)
{
	struct bitmap_ipmac *map = set->data;
	
	memset(map->members, 0,
	       (map->last_ip - map->first_ip + 1) * map->elem_size);
}

static int
bitmap_ipmac_head(struct ip_set *set, struct sk_buff *skb)
{
	const struct bitmap_ipmac *map = set->data;
	struct nlattr *nested;
	const struct ipmac *elem;
	uint32_t id, elements = 0, last = map->last_ip - map->first_ip;
	bool with_timeout = set->flags & IP_SET_FLAG_TIMEOUT;

	for (id = 0; id <= last; id++) {
		elem = bitmap_ipmac_elem(map, id);
		if (bitmap_ipmac_exist(elem, with_timeout))
			elements++;
	}

	nested = ipset_nest_start(skb, IPSET_ATTR_DATA);
	if (!nested)
		goto nla_put_failure;
	NLA_PUT_NET32(skb, IPSET_ATTR_IP, htonl(map->first_ip));
	NLA_PUT_NET32(skb, IPSET_ATTR_IP_TO, htonl(map->last_ip));
	NLA_PUT_NET32(skb, IPSET_ATTR_ELEMENTS, htonl(elements));
	NLA_PUT_NET32(skb, IPSET_ATTR_REFERENCES,
		      htonl(atomic_read(&set->ref) - 1));
	NLA_PUT_NET32(skb, IPSET_ATTR_MEMSIZE,
		      htonl((map->last_ip - map->first_ip + 1)
		      	    * map->elem_size));
	if (with_timeout)
		NLA_PUT_NET32(skb, IPSET_ATTR_TIMEOUT, htonl(map->timeout));
	ipset_nest_end(skb, nested);
	
	return 0;
nla_put_failure:
	return -EFAULT;
}

static int
bitmap_ipmac_list(struct ip_set *set,
		  struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct bitmap_ipmac *map = set->data;
	const struct ipmac *elem;
	struct nlattr *atd, *nested;
	uint32_t id, first = cb->args[2];
	uint32_t last = map->last_ip - map->first_ip;
	bool with_timeout = set->flags & IP_SET_FLAG_TIMEOUT;

	atd = ipset_nest_start(skb, IPSET_ATTR_ADT);
	if (!atd)
		return -EFAULT;
	for (; cb->args[2] <= last; cb->args[2]++) {
		id = cb->args[2];
		elem = bitmap_ipmac_elem(map, id);
		if (!bitmap_ipmac_exist(elem, with_timeout)) 
			continue;
		nested = ipset_nest_start(skb, IPSET_ATTR_DATA);
		if (!nested) {
			if (id == first) {
				nla_nest_cancel(skb, atd);
				return -EFAULT;
			} else
				goto nla_put_failure;
		}
		NLA_PUT_NET32(skb, IPSET_ATTR_IP,
			      htonl(map->first_ip + id));
		if (elem->match == MAC_FILLED)
			NLA_PUT(skb, IPSET_ATTR_ETHER, ETH_ALEN,
				elem->ether);
		if (with_timeout) {
			const struct ipmac_timeout *e =
				(const struct ipmac_timeout *)elem;
			uint32_t timeout = e->match == MAC_UNSET ? e->timeout
					: ip_set_timeout_get(e->timeout);
	
			NLA_PUT_NET32(skb, IPSET_ATTR_TIMEOUT,
				      htonl(timeout));
		}
		ipset_nest_end(skb, nested);
	}
	ipset_nest_end(skb, atd);
	/* Set listing finished */
	cb->args[2] = 0;
	
	return 0;

nla_put_failure:
	nla_nest_cancel(skb, nested);
	ipset_nest_end(skb, atd);
	return 0;
}

const struct ip_set_type_variant bitmap_ipmac __read_mostly = {
	.kadt	= bitmap_ipmac_kadt,
	.uadt	= bitmap_ipmac_uadt,
	.destroy = bitmap_ipmac_destroy,
	.flush	= bitmap_ipmac_flush,
	.head	= bitmap_ipmac_head,
	.list	= bitmap_ipmac_list,
};

static void
bitmap_ipmac_timeout_gc(unsigned long ul_set)
{
	struct ip_set *set = (struct ip_set *) ul_set;
	struct bitmap_ipmac *map = set->data;
	struct ipmac_timeout *elem;
	uint32_t id, last = map->last_ip - map->first_ip;
	
	/* We run parallel with other readers (test element)
	 * but adding/deleting new entries is locked out */
	read_lock_bh(&set->lock);
	for (id = 0; id <= last; id++) {
		elem = bitmap_ipmac_elem(map, id);
		if (elem->match == MAC_FILLED
		    && ip_set_timeout_expired(elem->timeout))
		    	elem->match = MAC_EMPTY;
	}
	read_unlock_bh(&set->lock);

	map->gc.expires = jiffies + IPSET_GC_PERIOD(map->timeout) * HZ;
	add_timer(&map->gc);
}

static inline void
bitmap_ipmac_timeout_gc_init(struct ip_set *set)
{
	struct bitmap_ipmac *map = set->data;

	init_timer(&map->gc);
	map->gc.data = (unsigned long) set;
	map->gc.function = bitmap_ipmac_timeout_gc;
	map->gc.expires = jiffies + IPSET_GC_PERIOD(map->timeout) * HZ;
	add_timer(&map->gc);
}

/* Create bitmap:ip,mac type of sets */

static const struct nla_policy
bitmap_ipmac_create_policy[IPSET_ATTR_CREATE_MAX+1] __read_mostly = {
	[IPSET_ATTR_IP]		= { .type = NLA_U32 },
	[IPSET_ATTR_IP_TO]	= { .type = NLA_U32 },
	[IPSET_ATTR_TIMEOUT]	= { .type = NLA_U32 },
};

static bool
init_map_ipmac(struct ip_set *set, struct bitmap_ipmac *map,
	       uint32_t first_ip, uint32_t last_ip)
{
	map->members = ip_set_alloc((last_ip - first_ip + 1) * map->elem_size,
				    GFP_KERNEL, &set->flags);
	if (!map->members)
		return false;
	map->first_ip = first_ip;
	map->last_ip = last_ip;

	set->data = map;
	set->family = AF_INET;
	
	return true;
}

static int
bitmap_ipmac_create(struct ip_set *set, struct nlattr *head, int len,
		    uint32_t flags)
{
	struct nlattr *tb[IPSET_ATTR_CREATE_MAX];
	uint32_t first_ip, last_ip, elements;
	struct bitmap_ipmac *map;

	if (nla_parse(tb, IPSET_ATTR_CREATE_MAX, head, len,
		      bitmap_ipmac_create_policy))
		return -IPSET_ERR_PROTOCOL;
	
	if (tb[IPSET_ATTR_IP])
		first_ip = ip_set_get_h32(tb[IPSET_ATTR_IP]);
	else
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_IP_TO]) {
		last_ip = ip_set_get_h32(tb[IPSET_ATTR_IP_TO]);
		if (first_ip > last_ip) {
			uint32_t tmp = first_ip;
			
			first_ip = last_ip;
			last_ip = tmp;
		}
	} else if (tb[IPSET_ATTR_CIDR]) {
		uint8_t cidr = nla_get_u8(tb[IPSET_ATTR_CIDR]);
		
		if (cidr >= 32)
			return -IPSET_ERR_INVALID_CIDR;
		last_ip = first_ip | ~HOSTMASK(cidr);
	} else
		return -IPSET_ERR_PROTOCOL;

	elements = last_ip - first_ip + 1;

	if (elements > IPSET_BITMAP_MAX_RANGE + 1)
		return -IPSET_ERR_BITMAP_RANGE_SIZE;

	set->variant = &bitmap_ipmac;
		
	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	if (tb[IPSET_ATTR_TIMEOUT]) {
		map->elem_size = sizeof(struct ipmac_timeout);
			       
		if (!init_map_ipmac(set, map, first_ip, last_ip)) {
			kfree(map);
			return -ENOMEM;
		}

		map->timeout = ip_set_get_h32(tb[IPSET_ATTR_TIMEOUT]);
		set->flags |= IP_SET_FLAG_TIMEOUT;
		
		bitmap_ipmac_timeout_gc_init(set);
	} else {		
		map->elem_size = sizeof(struct ipmac);

		if (!init_map_ipmac(set, map, first_ip, last_ip)) {
			kfree(map);
			return -ENOMEM;
		}
	}
	return 0;
}

struct ip_set_type bitmap_ipmac_type = {
	.name		= "bitmap:ip,mac",
	.protocol	= IPSET_PROTOCOL,
	.features	= IPSET_TYPE_IP,
	.family		= AF_INET,
	.revision	= 0,
	.create		= bitmap_ipmac_create,
	.me		= THIS_MODULE,
};

static int __init
bitmap_ipmac_init(void)
{
	return ip_set_type_register(&bitmap_ipmac_type);
}

static void __exit
bitmap_ipmac_fini(void)
{
	ip_set_type_unregister(&bitmap_ipmac_type);
}

module_init(bitmap_ipmac_init);
module_exit(bitmap_ipmac_fini);