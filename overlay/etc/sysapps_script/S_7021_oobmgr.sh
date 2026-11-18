#!/bin/sh
# Custom init hook: modern TLS cert for the stock web UI + a real root shell.
# Add this line to the kernel's init script list (on iDRAC6: /etc/init.d/postinit_adv.sh,
# after the stock sshd line and before the web-server line) -- the list is EXPLICIT,
# a new S_* file is NOT auto-run:
#     /etc/sysapps_script/S_7021_oobmgr.sh start
CERTDIR=/flash/data0/etc/certs
OPENSSL=/bin/openssl

regen_cert() {
	mkdir -p "$CERTDIR"
	# only replace an expired/missing cert, never a valid user-uploaded one
	if [ -f "$CERTDIR/host.cert" ] && $OPENSSL x509 -checkend 86400 -noout -in "$CERTDIR/host.cert" >/dev/null 2>&1; then
		return 0
	fi
	IP=$(ip -4 addr show 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | grep -v '^127' | head -1)
	[ -z "$IP" ] && IP=127.0.0.1
	HOST=$(hostname 2>/dev/null); [ -z "$HOST" ] && HOST=bmc
	CONF=/tmp/oob_cert.cnf
	printf '[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=%s\n[ext]\nsubjectAltName=IP:%s,DNS:%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' "$HOST" "$IP" "$HOST" > $CONF
	$OPENSSL req -x509 -newkey rsa:2048 -sha256 -nodes -days 7300 -keyout "$CERTDIR/host.key" -out "$CERTDIR/host.cert" -config $CONF >/dev/null 2>&1
	rm -f $CONF
}

start() {
	regen_cert
	# second sshd: UsePAM no bypasses the restricted CLI -> real /bin/sh (BusyBox).
	# Stock sshd on :22 is untouched. Put YOUR pubkey in /etc/oob/authorized_keys_root
	# and/or set a root password in /flash/data0/etc/shadow for password auth.
	[ -x /sbin/sshd ] && /sbin/sshd -f /etc/oob/sshd_config_root 2>>/var/log/oob_sshd.log
}
stop() { kill $(cat /var/run/sshd_root.pid 2>/dev/null) 2>/dev/null; }
case "$1" in start) start ;; stop) stop ;; restart|reset) stop; start ;; *) start ;; esac
