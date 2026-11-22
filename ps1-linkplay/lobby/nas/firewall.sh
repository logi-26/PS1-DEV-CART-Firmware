#!/bin/sh
# Net Play lobby firewall (container netplay_firewall, host network, NET_ADMIN only; the same as PS1 PLAZA's, for the
# lobby's networks): its containers can't open
# connections to the home network, the NAS itself, Tailscale or other private networks; replies to players and the
# containers' own networks still flow, and the tunnel reaches Cloudflare. The rules are checked again every minute
# (QTS can reload its firewall). "firewall.sh remove" deletes them.
#   firewall.sh SUBNET...      e.g. 172.31.250.0/24 172.31.251.0/24
IPT=iptables-legacy
PRIVATE="10.0.0.0/8 172.16.0.0/12 192.168.0.0/16 100.64.0.0/10 169.254.0.0/16"

rule() {	# $1: -A/-D/-C, $2: chain, then the rule
	op=$1; chain=$2; shift 2
	$IPT -w $op $chain "$@" 2> /dev/null
}

complete() {	# All the rules there?
	for net in $NETS; do
		for dst in $PRIVATE; do
			rule -C DOCKER-USER -s $net -d $dst -m conntrack --ctstate NEW -j DROP || return 1
		done
		rule -C INPUT -s $net -m conntrack --ctstate NEW -j DROP || return 1
		for other in $NETS; do
			rule -C DOCKER-USER -s $net -d $other -j RETURN || return 1
		done
	done
	return 0
}

apply() {	# (any rule missing: all of them again, in order)
	complete && return
	remove
	for net in $NETS; do
		# Forwarded (the router, other machines, other containers): new connections to private addresses dropped...
		for dst in $PRIVATE; do
			$IPT -w -I DOCKER-USER 1 -s $net -d $dst -m conntrack --ctstate NEW -j DROP
		done
		# The NAS itself (its LAN and container gateway addresses): new connections dropped
		$IPT -w -I INPUT 1 -s $net -m conntrack --ctstate NEW -j DROP
	done
	for net in $NETS; do		# ...but the game's own networks between themselves (before the drops)
		for other in $NETS; do
			$IPT -w -I DOCKER-USER 1 -s $net -d $other -j RETURN
		done
	done
	echo "$(date '+%H:%M:%S') rules applied for $NETS"
}

remove() {
	for net in $NETS; do
		for dst in $PRIVATE; do
			while rule -D DOCKER-USER -s $net -d $dst -m conntrack --ctstate NEW -j DROP; do :; done
		done
		while rule -D INPUT -s $net -m conntrack --ctstate NEW -j DROP; do :; done
		for other in $NETS; do
			while rule -D DOCKER-USER -s $net -d $other -j RETURN; do :; done
		done
	done
}

if [ "$1" = "remove" ]; then
	shift
	NETS="$*"
	remove
	echo "rules removed"
	exit 0
fi
NETS="$*"
[ -n "$NETS" ] || { echo "usage: $0 SUBNET... | remove SUBNET..."; exit 1; }
trap 'exit 0' TERM INT
while true; do
	apply
	sleep 60 &
	wait $!
done
