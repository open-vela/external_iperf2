# common test setup
set -o pipefail
set -x

lo=localhost
ip=127.0.0.1
ip6=::1

port=$(expr 5000 + $0 : '.*/t\([0-9]*\)_')
lport=$(expr 6000 - $0 : '.*/t\([0-9]*\)_')

iperf() {
    src/iperf -p $port "$@" 2>&1 \
    | awk '/unrecognized|ignoring|failed|not valid/{e=1};{print};END{exit e}'
}
