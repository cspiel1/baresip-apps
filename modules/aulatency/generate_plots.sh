#!/bin/bash

evaluate_json () {
    ### Generate plot
    cat aulat-2.json | jq -r '.traceEvents[] | select (.ph == "P") | .args.line' > aulat.dat

    ### Generate min/max statistics
    min=$(cat aulat-2.json | jq -r '.traceEvents[] | select (.ph == "P") | .args.line' | awk 'BEGIN{a=90000; FS=","}{if ($2<0+a) a=$2} END{print a}')
    max=$(cat aulat-2.json | jq -r '.traceEvents[] | select (.ph == "P") | .args.line' | awk 'BEGIN{a=0; FS=","}{if ($2>0+a) a=$2} END{print a}')
    echo "$min, $max" >> aulat-stat.dat

    ./aulat.plot
    if [ ! -d plots ]; then
        mkdir plots
    fi

    mv aulat.eps plots
    mv aulat-rhwh-jitter.eps plots
    mv *.png plots
}


peer=$1
cd $(dirname $0)
script=$(basename $0)

if [[ "$peer" != "-remote" ]]; then
    if ! which jq; then
        echo "Install jq"
        exit 1
    fi

    if ! which gnuplot; then
        echo "Install gnuplot"
        exit 1
    fi
fi

if [ ! -z $peer ]
then
    if [[ "$peer" == "-remote" ]]; then
        remote="yes"
        export DISPLAY=:0
    else
        scp -r .baresip $peer:/tmp
        scp -r .baresip2 $peer:/tmp
        scp $script $peer:/tmp
        ssh -X $peer "/tmp/$script -remote" ||
            { echo "$script failed at $peer"; exit 1; }
        scp $peer:/tmp/aulat-2.json . ||
            { echo "No /tmp/aulat-2.json on $peer"; exit 1; }
        evaluate_json
        exit 0
    fi
else
    cp -r .baresip /tmp
    cp -r .baresip2 /tmp
fi

rm aulat*.json
rm aulat.dat

target=$(ip -f inet addr | grep -E "\<inet\>" | grep -v 127.0.0.1 | \
    head -n 1 | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+\.[0-9]\+' | head -n 1)

path=$(which baresip | sed -e "s_bin/baresip__")
sed -i -e "s+/usr/local/+$path+g" /tmp/.baresip/config
sed -i -e "s+/usr/local/+$path+g" /tmp/.baresip2/config

echo "target=$target"

trap "killall -q baresip" EXIT

export LD_LIBRARY_PATH="$path/lib"

### Start baresip with aubridge
baresip -f /tmp/.baresip2 &

### Start baresip with aulatency
baresip -f /tmp/.baresip &
sleep 1

### Invoke call
echo "/dial $target:15060" | nc -N localhost 25555
sleep 20
echo "/quit" | nc -N localhost 25555
echo "/quit" | nc -N localhost 15555

if [ -z $remote ]; then
    evaluate_json
fi
