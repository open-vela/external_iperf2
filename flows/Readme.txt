The following steps needs to be done before running pyflows test:

1) Install Python 3.7 and above

2) sudo dnf install python3-matplotlib

3) Update your ".bashrc" and add the location of your flows files "/your_local_dir/iperf2-code/flows" to PYTHONPATH:
export PYTHONPATH=/your_local_dir/iperf2-code/flows:$PYTHONPATH
echo $PYTHONPATH

4) compile the following moudlues:
cd /your_local_dir/iperf2-code/flows
python3 -m py_compile aeroflex.py  flows.py  netlink.py  rf_topology.py  ssh_nodes.py

5) Configure the IP addresses and LAN addresses in the router_latency.py

6) Run the test:
cd /your_local_dir/iperf2-code/flows
python3 router_latency.py

