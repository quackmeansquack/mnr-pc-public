#!/bin/bash
# Back up the private server: the PLGarage database and the uploaded creations (UGC) into ~/srv/backups/<date>,
# keeping the newest 7. Run as root (mysqldump). backup-loop.sh runs it every night; it can also be run by hand.
S=/home/mnr-server/srv
B=$S/backups
T=$(date +%Y-%m-%d_%H%M)
mkdir -p "$B/$T"
mysqldump --single-transaction PLGarage | gzip > "$B/$T/PLGarage.sql.gz" && tar czf "$B/$T/UGC.tgz" -C $S/plgarage-app UGC \
    && echo "$(date '+%F %T') backup $T: $(du -sh "$B/$T" | cut -f1)" >> $B/backup.log \
    || { echo "$(date '+%F %T') backup $T FAILED" >> $B/backup.log; exit 1; }
ls -1d $B/20*/ 2>/dev/null | head -n -7 | xargs -r rm -rf   # (the newest 7 stay)
