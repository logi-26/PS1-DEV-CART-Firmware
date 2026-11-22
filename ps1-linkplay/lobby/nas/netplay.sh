#!/bin/sh
# PS1 DEV CART Net Play lobby server on a QNAP NAS (Container Station), running all the time. The same hardened layout
# as the PS1 PLAZA server (PS1-DEV-CART-Plaza/server/nas/plaza.sh), on networks of its own.
#   ./netplay.sh start       everything started (and again after a NAS restart: restart policies)
#   ./netplay.sh stop        everything stopped and removed (the firewall rules stay until "firewall-remove" or a restart)
#   ./netplay.sh restart     after copying a new netplay_server.py to app/
#   ./netplay.sh status | logs [lines] | proxy-logs | firewall-logs
#   ./netplay.sh firewall-remove
#
# Containers:
#   netplay_server         the lobby server: only on netplay_internal, a sealed network (--internal): it can reach
#                          nothing, not the internet, not the home network, not the NAS
#   netplay_server_proxy   nginx (unprivileged): the only one with a published port, the lobby port (all interfaces, the
#                          one forwarded on the router), with the player's address first (PROXY protocol, trusted by
#                          the server only from the proxy's fixed address)
#   netplay_firewall       host network, NET_ADMIN only: keeps iptables rules so that nothing on the lobby's networks
#                          can open a connection to the home network, the NAS itself or Tailscale (firewall.sh)
# All of them: read-only file system, no capabilities (but the firewall's NET_ADMIN/NET_RAW), no new privileges,
# unprivileged users, memory/CPU/process limits, logs limited in size, no shared folders (only their own files,
# read-only).
D=/share/CACHEDEV2_DATA/.qpkg/container-station/bin/docker
DIR=$(cd "$(dirname "$0")" && pwd)
NAME=netplay_server
INTERNAL=netplay_internal
EDGE=netplay_edge
INTERNAL_NET=172.31.252.0/24
EDGE_NET=172.31.253.0/24
INTERNAL_RANGE=172.31.252.128/25		# Addresses given by Docker (the proxy's fixed one is below)
PROXY_IP=172.31.252.2
IMG=python:3.12-slim
PROXY_IMG=nginxinc/nginx-unprivileged:1.27-alpine
FIREWALL_IMG=netplay-firewall:latest
LOBBY_PORT=${LOBBY_PORT:-7031}
LOCKED="--read-only --cap-drop ALL --security-opt no-new-privileges"
PYTHON="$LOCKED --user 65534:65534 -e PYTHONDONTWRITEBYTECODE=1"
LOGS="--log-opt max-size=5m --log-opt max-file=3"
CONTAINERS="${NAME}_proxy $NAME"

networks() {
	$D network inspect $INTERNAL > /dev/null 2>&1 || \
		$D network create --internal --subnet $INTERNAL_NET --ip-range $INTERNAL_RANGE $INTERNAL > /dev/null || exit 1
	$D network inspect $EDGE > /dev/null 2>&1 || $D network create --subnet $EDGE_NET $EDGE > /dev/null || exit 1
}

firewall() {
	$D image inspect $FIREWALL_IMG > /dev/null 2>&1 || \
		DOCKER_BUILDKIT=0 $D build -q -t $FIREWALL_IMG -f "$DIR/Dockerfile.firewall" "$DIR" > /dev/null || exit 1
	$D rm -f netplay_firewall > /dev/null 2>&1
	$D run -d --name netplay_firewall --restart unless-stopped --network host $LOCKED --tmpfs /run:size=1m \
		--cap-add NET_ADMIN --cap-add NET_RAW --memory 32m --memory-swap 32m --pids-limit 16 $LOGS \
		$FIREWALL_IMG $INTERNAL_NET $EDGE_NET > /dev/null || exit 1
}

case "$1" in
start)
	networks
	firewall
	for c in $CONTAINERS; do $D rm -f $c > /dev/null 2>&1; done
	$D run -d --name $NAME --restart unless-stopped --network $INTERNAL $PYTHON \
		--memory 128m --memory-swap 128m --cpus 1 --pids-limit 64 $LOGS \
		-v "$DIR/app/netplay_server.py:/app/netplay_server.py:ro" \
		$IMG python -u /app/netplay_server.py --port 7031 --proxy-from $PROXY_IP $SERVER_ARGS > /dev/null || exit 1
	$D create --name ${NAME}_proxy --restart unless-stopped --network $EDGE $LOCKED --tmpfs /tmp:size=8m \
		--memory 64m --memory-swap 64m --cpus 0.5 --pids-limit 32 $LOGS \
		-v "$DIR/nginx.conf:/etc/nginx/nginx.conf:ro" \
		-p $LOBBY_PORT:7031/tcp $PROXY_IMG > /dev/null || exit 1
	$D network connect --ip $PROXY_IP $INTERNAL ${NAME}_proxy || exit 1
	$D start ${NAME}_proxy > /dev/null || exit 1
	echo "started: lobby port $LOBBY_PORT"
	;;
stop)
	for c in $CONTAINERS; do $D rm -f $c > /dev/null 2>&1; done
	echo "stopped (firewall rules kept: ./netplay.sh firewall-remove)"
	;;
restart)
	"$0" stop > /dev/null
	"$0" start
	;;
status)
	$D ps -a --filter name=netplay_ --format "{{.Names}}  {{.Status}}  {{.Ports}}"
	$D stats --no-stream --format "{{.Name}}: CPU {{.CPUPerc}}  memory {{.MemUsage}}  processes {{.PIDs}}" \
		$($D ps -q --filter name=netplay_) 2>/dev/null
	;;
logs)
	$D logs --tail ${2:-40} $NAME
	;;
proxy-logs)
	$D logs --tail ${2:-20} ${NAME}_proxy
	;;
firewall-logs)
	$D logs --tail ${2:-20} netplay_firewall
	;;
firewall-remove)
	$D rm -f netplay_firewall > /dev/null 2>&1
	$D run --rm --network host $LOCKED --tmpfs /run:size=1m --cap-add NET_ADMIN --cap-add NET_RAW --entrypoint /bin/sh \
		$FIREWALL_IMG \
		/firewall.sh remove $INTERNAL_NET $EDGE_NET
	;;
*)
	echo "usage: $0 start|stop|restart|status|logs [lines]|proxy-logs|firewall-logs|firewall-remove"
	;;
esac
