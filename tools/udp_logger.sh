#!/bin/bash
# Pyxis UDP Log Receiver
# Captures multicast logs from the T-Deck on 239.0.99.99:9999
# Requires: socat (apt install socat)

LOG_DIR="$HOME/pyxis-logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/$(date +%Y%m%d-%H%M%S).log"
echo "Listening on multicast 239.0.99.99:9999, logging to $LOG_FILE"
socat -u UDP4-RECV:9999,ip-add-membership=239.0.99.99:0.0.0.0,reuseaddr - | while IFS= read -r line; do
    echo "$(date '+%H:%M:%S') $line" | tee -a "$LOG_FILE"
done
