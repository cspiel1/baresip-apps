#!/usr/bin/gnuplot
#
# Copyright (C) 2023 commend.com - Christian Spielberger


# Choose your preferred gnuplot terminal or use e.g. evince to view the
# ajb.eps!

#set terminal qt persist
set terminal postscript eps size 15,10 enhanced color
#set terminal png size 1280,480
set datafile separator ","
set key outside
set xlabel "time / [ms]"

set output 'aulat.eps'
#set output 'aulat.png'
set ylabel "[ms]"

plot \
'aulat.dat' using 1:($2/1000) title 'latency' with linespoints lc "light-grey", \
'aulat.dat' using 1:($3/1000) title 'ev. latency' with linespoints lc "orange", \
'aulat.dat' using 1:($5/1000) title 'read jitter' with linespoints lc "blue", \
'aulat.dat' using 1:($7/1000) title 'write jitter' with linespoints lc "green"

set output 'aulat-rhwh-jitter.eps'
#set output 'aulat-rhwh-jitter.png'
set ylabel "[us]"
plot \
'aulat.dat' using 1:($5) title 'read jitter' with linespoints lc "blue", \
'aulat.dat' using 1:($7) title 'write jitter' with linespoints lc "green"

set output 'aulat-stat.png'
set terminal png size 800,400
set yrange [0:]
set xlabel ""
set ylabel "[ms]"
set style data histogram
set style fill solid
set style histogram cluster gap 1
set xtics 0,1
set boxwidth 0.9
unset key
plot \
'aulat-stat.dat' using ($1/1000) lc "dark-green", \
'aulat-stat.dat' using ($2/1000) lc "dark-red"

