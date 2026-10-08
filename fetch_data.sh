#!/bin/bash

set -euo pipefail


# wikipedia titles dump sept
wget -c "https://dumps.wikimedia.org/enwiki/20260901/enwiki-20260901-all-titles-in-ns0.gz"
gunzip -kf "enwiki-20260901-all-titles-in-ns0.gz"
# no header, lines with 1..200 bytes, just printable ascii, sort unique shuff
echo "" >> "enwiki-20260901-all-titles-in-ns0" # term \n
tail -n +2 "enwiki-20260901-all-titles-in-ns0" \
| LC_ALL=C grep -a '^[ -~]\{1,200\}$' \
| LC_ALL=C sort -u \
> titles_ascii_sorted.txt
shuf titles_ascii_sorted.txt > titles_ascii.txt


# wikipedia titles dump sept ch and jp
wget -c "https://dumps.wikimedia.org/zhwiki/20260901/zhwiki-20260901-all-titles-in-ns0.gz"
wget -c "https://dumps.wikimedia.org/jawiki/20260901/jawiki-20260901-all-titles-in-ns0.gz"
gunzip -kf "zhwiki-20260901-all-titles-in-ns0.gz"
gunzip -kf "jawiki-20260901-all-titles-in-ns0.gz"
echo "" >> "zhwiki-20260901-all-titles-in-ns0"
echo "" >> "jawiki-20260901-all-titles-in-ns0"
# perl: match 200 characters and then up to 3 code unit sequence characters so we don't create broken text
cat \
  <(tail -n +2 "zhwiki-20260901-all-titles-in-ns0") \
  <(tail -n +2 "jawiki-20260901-all-titles-in-ns0") \
| perl -pe 's/^(.{200}[\x80-\xBF]{0,3}).*/$1/' \
| LC_ALL=C sort -u \
> titles_cjk_sorted.txt
shuf titles_cjk_sorted.txt > titles_cjk.txt


# generate ca 350mb of dates rand 2010 to 2020 in format [used in open library
# data dump dates, but dataset small]: YYYY-MM-DDTHH:MM:SS.ffffff
nr_dates=$(( 350 * 1000 * 1000 / 27 ))
# random draw timestamps from region, split, to date, ...
shuf -r -n "$nr_dates" -i 1262304000000000-1609459199999999 \
| awk '{ printf "@%d.%06d\n", int($1 / 1000000), $1 % 1000000 }' \
| TZ=UTC date -f - '+%Y-%m-%dT%H:%M:%S.%6N' \
| LC_ALL=C sort -u \
> dates_sorted.txt
shuf dates_sorted.txt > dates.txt
