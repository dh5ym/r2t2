/*
 * (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2006 Netfilter Core Team <coreteam@netfilter.org>
 * (C) 2011 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <net/secure_seq.h>
#include <net/checksum.h>
#include <net/route.h>
#include <net/ip.h>

#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_nat_core.h>
#include <net/netfilter/nf_nat_l3proto.h>
#include <net/netfilter/nf_nat_l4proto.h>

static const struct nf_nat_l3proto nf_nat_l3proto_ipv4;

#ifdef CONFIG_XFRM
static void nf_nat_ipv4_decode_session(struct sk_buff *skb,
				       const struct nf_conn *ct,
				       enum ip_conntrack_dir dir,
				       unsigned long statusbit,
				       struct flowi *fl)
{
	const struct nf_conntrack_tuple *t = &ct->tuplehash[dir].tuple;
	struct flowi4 *fl4 = &fl->u.ip4;

	if (ct->status & statusbit) {
		fl4->daddr = t->dst.u3.ip;
		if (t->dst.protonum == IPPROTO_TCP ||
		    t->dst.protonum == IPPROTO_UDP ||
		    t->dst.protonum == IPPROTO_UDPLITE ||
		    t->dst.protonum == IPPROTO_DCCP ||
		    t->dst.protonum == IPPROTO_SCTP)
			fl4->fl4_dport = t->dst.u.all;
	}

	statusbit ^= IPS_NAT_MASK;

	if (ct->status & statusbit) {
		fl4->saddr = t->src.u3.ip;
		if (t->dst.protonum == IPPROTO_TCP ||
		    t->dst.protonum == IPPROTO_UDP ||
		    t->dst.protonum == IPPROTO_UDPLITE ||
		    t->dst.protonum == IPPROTO_DCCP ||
		    t->dst.protonum == IPPROTO_SCTP)
			fl4->fl4_sport = t->src.u.all;
	}
}
#endif /* CONFIG_XFRM */

static bool nf_nat_ipv4_in_range(const struct nf_conntrack_tuple *t,
				 const struct nf_nat_range *range)
{
	return ntohl(t->src.u3.ip) >= ntohl(range->min_addr.ip) &&
	       ntohl(t->src.u3.ip) <= ntohl(range->max_addr.ip);
}

static u32 nf_nat_ipv4_secure_port(const struct nf_conntrack_tuple *t,
				   __be16 dport)
{
	return secure_ipv4_port_ephemeral(t->src.u3.ip, t->dst.u3.ip, dport);
}

static bool nf_nat_ipv4_manip_pkt(struct sk_buff *skb,
				  unsigned int iphdroff,
				  const struct nf_nat_l4proto *l4proto,
				  const struct nf_conntrack_tuple *target,
				  enum nf_nat_manip_type maniptype)
{
	struct iphdr *iph;
	unsigned int hdroff;

	if (!skb_make_writable(skb, iphdroff + sizeof(*iph)))
		return false;

	iph = (void *)skb->data + iphdroff;
	hdroff = iphdroff + iph->ihl * 4;

	if (!l4proto->manip_pkt(skb, &nf_nat_l3proto_ipv4, iphdroff, hdroff,
				target, maniptype))
		return false;
	iph = (void *)skb->data + iphdroff;

	if (maniptype == NF_NAT_MANIP_SRC) {
		csum_replace4(&iph->check, iph->saddr, target->src.u3.ip);
		iph->saddr = target->src.u3.ip;
	} else {
		csum_replace4(&iph->check, iph->daddr, target->dst.u3.ip);
		iph->daddr = target->dst.u3.ip;
	}
	return true;
}

static void nf_nat_ipv4_csum_update(struct sk_buff *skb,
				    unsigned int iphdroff, __sum16 *check,
				    const struct nf_conntrack_tuple *t,
				    enum nf_nat_manip_type maniptype)
{
	struct iphdr *iph = (struct iphdr *)(skb->data + iphdroff);
	__be32 oldip, newip;

	if (maniptype == NF_NAT_MANIP_SRC) {
		oldip = iph->saddr;
		newip = t->src.u3.ip;
	} else {
		oldip = iph->daddr;
		newip = t->dst.u3.ip;
	}
	inet_proto_csum_replace4(check, skb, oldip, newip, 1);
}

static void nf_nat_ipv4_csum_recalc(struct sk_buff *skb,
				    u8 proto, void *data, __sum16 *check,
				    int datalen, int oldlen)
{
	const struct iphdr *iph = ip_hdr(skb);
	struct rtable *rt = skb_rtable(skb);

	if (skb->ip_summed != CHECKSUM_PARTIAL) {
		if (!(rt->rt_flags & RTCF_LOCAL) &&
		    (!skb->dev || skb->dev->features & NETIF_F_V4_CSUM)) {
			skb->ip_summed = CHECKSUM_PARTIAL;
			skb->csum_start = skb_headroom(skb) +
					  skb_network_offset(skb) +
					  ip_hdrlen(skb);
			skb->csum_offset = (void *)check - data;
			*check = ~csum_tcpudp_magic(iph->saddr, iph->daddr,
						    datalen, proto, 0);
		} else {
			*check = 0;
			*check = csum_tcpudp_magic(iph->saddr, iph->daddr,
						   datalen, proto,
						   csum_partial(data, datalen,
								0));
			if (proto == IPPROTO_UDP && !*check)
				*check = CSUM_MANGLED_0;
		}
	} else
		inet_proto_csum_replace2(check, skb,
					 htons(oldlen), htons(datalen), 1);
}

#if IS_ENABLED(CONFIG_NF_CT_NETLINK)
static int nf_nat_ipv4_nlattr_to_range(struct nlattr *tb[],
				       struct nf_nat_range *range)
{
	if (tb[CTA_NAT_V4_MINIP]) {
		range->min_addr.ip = nla_get_be32(tb[CTA_NAT_V4_MINIP]);
		range->flags |= NF_NAT_RANGE_MAP_IPS;
	}

	if (tb[CTA_NAT_V4_MAXIP])
		range->max_addr.ip = nla_get_be32(tb[CTA_NAT_V4_MAXIP]);
	else
		range->max_addr.ip = range->min_addr.ip;

	return 0;
}
#endif

static const struct nf_nat_l3proto nf_nat_l3proto_ipv4 = {
	.l3proto		= NFPROTO_IPV4,
	.in_range		= nf_nat_ipv4_in_range,
	.secure_port		= nf_nat_ipv4_secure_port,
	.manip_pkt		= nf_nat_ipv4_manip_pkt,
	.csum_update		= nf_nat_ipv4_csum_update,
	.csum_recalc		= nf_nat_ipv4_csum_recalc,
#if IS_ENABLED(CONFIG_NF_CT_NETLINK)
	.nlattr_to_range	= nf_nat_ipv4_nlattr_to_range,
#endif
#ifdef CONFIG_XFRM
	.decode_session		= nf_nat_ipv4_decode_session,
#endif
};

int nf_nat_icmp_reply_translation(struct sk_buff *skb,
				  struct nf_conn *ct,
				  enum ip_conntrack_info ctinfo,
				  unsigned int hooknum)
{
	struct {
		struct icmphdr	icmp;
		struct iphdr	ip;
	} *inside;
	enum ip_conntrack_dir dir = CTINFO2DIR(ctinfo);
	enum nf_nat_manip_type manip = HOOK2MANIP(hooknum);
	unsigned int hdrlen = ip_hdrlen(skb);
	const struct nf_nat_l4proto *l4proto;
	struct nf_conntrack_tuple target;
	unsigned long statusbit;

	NF_CT_ASSERT(ctinfo == IP_CT_RELATED || ctinfo == IP_CT_RELATED_REPLY);

	if (!skb_make_writable(skb, hdrlen + sizeof(*inside)))
		return 0;
	if (nf_ip_checksum(skb, hooknum, hdrlen, 0))
		return 0;

	inside = (void *)skb->data + hdrlen;
	if (inside->icmp.type == ICMP_REDIRECT) {
		if ((ct->status & IPS_NAT_DONE_MASK) != IPS_NAT_DONE_MASK)
			return 0;
		if (ct->status & IPS_NAT_MASK)
			return 0;
	}

	if (manip == NF_NAT_MANIP_SRC)
		statusbit = IPS_SRC_NAT;
	else
		statusbit = IPS_DST_NAT;

	/* Invert if this is reply direction */
	if (dir == IP_CT_DIR_REPLY)
		statusbit ^= IPS_NAT_MASK;

	if (!(ct->status & statusbit))
		return 1;

	l4proto = __nf_nat_l4proto_find(NFPROTO_IPV4, inside->ip.protocol);
	if (!nf_nat_ipv4_manip_pkt(skb, hdrlen + sizeof(inside->icmp),
				   l4proto, &ct->tuplehash[!dir].tuple, !manip))
		return 0;

	if (skb->ip_summed != CHECKSUM_PARTIAL) {
		/* Reloading "inside" here since manip_pkt may reallocate */
		inside = (void *)skb->data + hdrlen;
		inside->icmp.checksum = 0;
		inside->icmp.checksum =
			csum_fold(skb_checksum(skb, hdrlen,
					       skb->len - hdrlen, 0));
	}

	/* Change outer to look like the reply to an incoming packet */
	nf_ct_invert_tuplepr(&target, &ct->tuplehash[!dir].tuple);
	l4proto = __nf_nat_l4proto_find(NFPROTO_IPV4, 0);
	if (!nf_nat_ipv4_manip_pkt(skb, 0, l4proto, &target, manip))
		return 0;

	return 1;
}
EXPORT_SYMBOL_GPL(nf_nat_icmp_reply_translation);

static int __init nf_nat_l3proto_ipv4_init(void)
{
	int err;

	err = nf_nat_l4proto_register(NFPROTO_IPV4, &nf_nat_l4proto_icmp);
	if (err < 0)
		goto err1;
	err = nf_nat_l3proto_register(&nf_nat_l3proto_ipv4);
	if (err < 0)
		goto err2;
	return err;

err2:
	nf_nat_l4proto_unregister(NFPROTO_IPV4, &nf_nat_l4proto_icmp);
err1:
	return err;
}

static void __exit nf_nat_l3proto_ipv4_exit(void)
{
	nf_nat_l3proto_unregister(&nf_nat_l3proto_ipv4);
	nf_nat_l4proto_unregister(NFPROTO_IPV4, &nf_nat_l4proto_icmp);
}

MODULE_LICENSE("GPL");
MODULE_ALIAS("nf-nat-" __stringify(AF_INET));

module_init(nf_nat_l3proto_ipv4_init);
module_exit(nf_nat_l3proto_ipv4_exit);