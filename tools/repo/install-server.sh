#!/bin/sh
# install-server.sh - set up a Linux server that hosts a Banana OS package
# repository (for `apt` and the App Store on Banana OS).
#
#   sudo ./install-server.sh [options]
#
#   --dir DIR        where the repository lives        (default /srv/banana-repo)
#   --name NAME      the repository's name             (default banana)
#   --domain NAME    the server's DNS name, for nginx  (default: any name, port 80)
#   --email ADDR     with --domain: get an HTTPS certificate (Let's Encrypt, certbot)
#   --no-nginx       no nginx: a systemd service runs `banana-repo serve` instead
#   --port N         its port with --no-nginx          (default 8080)
#   --owner USER     who may add packages              (default: the user running sudo)
#   --dry-run        only print what would be done
#
# Debian, Ubuntu, Fedora, RHEL-likes and Arch. What it does:
#   1. installs python3 (and nginx, certbot) with the system's package manager
#   2. puts banana-repo in /usr/local/bin
#   3. makes the repository (banana-repo init) owned by --owner; the signing
#      key goes to that user's ~/.config/banana-repo/NAME.key - never served
#   4. serves the folder: an nginx site (the index never cached, hidden files
#      refused), or a systemd service
# Then, as --owner:  banana-repo add DIR app.bpk   and   banana-repo key DIR URL
set -eu

DIR=/srv/banana-repo
NAME=banana
DOMAIN=
EMAIL=
NGINX=1
PORT=8080
OWNER=${SUDO_USER:-}
DRY=0
HERE=$(cd "$(dirname "$0")" && pwd)

while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR=$2; shift 2 ;;
        --name) NAME=$2; shift 2 ;;
        --domain) DOMAIN=$2; shift 2 ;;
        --email) EMAIL=$2; shift 2 ;;
        --no-nginx) NGINX=0; shift ;;
        --port) PORT=$2; shift 2 ;;
        --owner) OWNER=$2; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "install-server.sh: unknown option $1 (--help)" >&2; exit 1 ;;
    esac
done

run() {
    if [ "$DRY" = 1 ]; then echo "+ $*"; else "$@"; fi
}
# writes stdin to a file (or shows it)
put() {
    if [ "$DRY" = 1 ]; then echo "+ write $1:"; sed 's/^/|   /'; else cat > "$1"; fi
}

case "$NAME" in *[!A-Za-z0-9._-]*|"") echo "install-server.sh: --name: letters, digits, . _ -" >&2; exit 1 ;; esac
case "$PORT" in *[!0-9]*|"") echo "install-server.sh: --port: a number" >&2; exit 1 ;; esac
case "$DIR" in /*) ;; *) echo "install-server.sh: --dir must be an absolute path" >&2; exit 1 ;; esac
if [ -n "$EMAIL" ] && [ -z "$DOMAIN" ]; then echo "install-server.sh: --email needs --domain (HTTPS is for a DNS name)" >&2; exit 1; fi
if [ -n "$EMAIL" ] && [ "$NGINX" = 0 ]; then echo "install-server.sh: HTTPS (--email) is done with nginx - drop --no-nginx" >&2; exit 1; fi
if [ "$DRY" = 0 ] && [ "$(id -u)" != 0 ]; then echo "install-server.sh: run it as root (sudo), or with --dry-run" >&2; exit 1; fi
if [ -z "$OWNER" ] || [ "$OWNER" = root ]; then
    echo "install-server.sh: say who adds packages: --owner <user> (not root: the signing key is theirs)" >&2; exit 1
fi
if ! id "$OWNER" >/dev/null 2>&1; then echo "install-server.sh: no user $OWNER" >&2; exit 1; fi
[ -f "$HERE/banana-repo" ] || { echo "install-server.sh: banana-repo is not next to this script" >&2; exit 1; }
OWNER_HOME=$(getent passwd "$OWNER" | cut -d: -f6)

# ── 1. packages ──
PKGS=python3
[ "$NGINX" = 1 ] && PKGS="$PKGS nginx"
if [ -n "$EMAIL" ]; then
    if command -v apt-get >/dev/null 2>&1; then PKGS="$PKGS certbot python3-certbot-nginx"
    else PKGS="$PKGS certbot python3-certbot-nginx"; fi
fi
if command -v apt-get >/dev/null 2>&1; then
    run apt-get update
    run env DEBIAN_FRONTEND=noninteractive apt-get install -y $PKGS
elif command -v dnf >/dev/null 2>&1; then
    run dnf install -y $PKGS
elif command -v pacman >/dev/null 2>&1; then
    [ -n "$EMAIL" ] && PKGS=$(echo "$PKGS" | sed 's/python3-certbot-nginx/certbot-nginx/')
    run pacman -Sy --noconfirm --needed $(echo "$PKGS" | sed 's/python3/python/')
else
    echo "install-server.sh: no apt-get, dnf or pacman - install $PKGS yourself, then run again" >&2
    exit 1
fi

# ── 2. the tool ──
run install -m 755 "$HERE/banana-repo" /usr/local/bin/banana-repo

# ── 3. the repository ──
if [ -f "$DIR/.banana-repo.json" ]; then
    echo "install-server.sh: $DIR is a repository already - kept as it is"
else
    run mkdir -p "$DIR"
    run chown "$OWNER": "$DIR"
    run chmod 755 "$DIR"
    run su -s /bin/sh "$OWNER" -c "/usr/local/bin/banana-repo init '$DIR' --name '$NAME'"
fi

# ── 4. serving it ──
if [ "$NGINX" = 1 ]; then
    SERVER_NAME=${DOMAIN:-_}
    if [ -d /etc/nginx/sites-available ]; then
        SITE=/etc/nginx/sites-available/banana-repo
        LINK=/etc/nginx/sites-enabled/banana-repo
    else
        SITE=/etc/nginx/conf.d/banana-repo.conf
        LINK=
    fi
    LISTEN="listen 80;
    listen [::]:80;"
    [ -z "$DOMAIN" ] && LISTEN="listen 80 default_server;
    listen [::]:80 default_server;"
    put "$SITE" <<EOF
# Banana OS package repository (made by install-server.sh)
server {
    $LISTEN
    server_name $SERVER_NAME;
    root $DIR;
    autoindex off;

    # the configuration and temporary files are not for anyone
    location ~ /\\. { deny all; return 404; }

    # the index and its signature change with every upload
    location ~ ^/Packages(\\.sig)?\$ {
        add_header Cache-Control "no-cache";
        default_type text/plain;
    }
    location ~ \\.bpk\$ {
        default_type application/octet-stream;
        add_header Cache-Control "public, max-age=86400";
    }
    location = /repo.pub { default_type text/plain; }
}
EOF
    if [ -n "$LINK" ]; then
        # the distribution's default site would answer instead of ours
        [ -z "$DOMAIN" ] && [ -e /etc/nginx/sites-enabled/default ] && run rm /etc/nginx/sites-enabled/default
        run ln -sf "$SITE" "$LINK"
    fi
    run nginx -t
    if command -v systemctl >/dev/null 2>&1; then
        run systemctl enable nginx
        run systemctl reload-or-restart nginx
    else
        run nginx -s reload
    fi
    if [ -n "$EMAIL" ]; then
        run certbot --nginx -d "$DOMAIN" -m "$EMAIL" --agree-tos --non-interactive --redirect
    fi
    if [ -n "$EMAIL" ]; then URL="https://$DOMAIN"; elif [ -n "$DOMAIN" ]; then URL="http://$DOMAIN"; else URL="http://<this server>"; fi
else
    put /etc/systemd/system/banana-repo.service <<EOF
[Unit]
Description=Banana OS package repository ($DIR)
After=network-online.target
Wants=network-online.target

[Service]
User=$OWNER
ExecStart=/usr/local/bin/banana-repo serve $DIR --port $PORT
Restart=on-failure
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=read-only
ReadOnlyPaths=$DIR
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
    run systemctl daemon-reload
    run systemctl enable --now banana-repo.service
    URL="http://${DOMAIN:-<this server>}:$PORT"
fi

cat <<EOF

Done. The repository is $DIR, served at $URL
As $OWNER:
    banana-repo add $DIR app.bpk          publish (or upgrade) packages
    banana-repo key $DIR $URL       the line for Banana OS
Back up the signing key: $OWNER_HOME/.config/banana-repo/$NAME.key
On Banana OS: apt add-repo $URL key=<key>   then   apt update   (or the App Store)
EOF
