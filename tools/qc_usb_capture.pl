#!/usr/bin/perl
# qc_usb_capture.pl - USB-HID-Mitschnitt des Quad Cortex per usbmon (fuer Weg A:
#   echtes Cortex Control unter Windows/QEMU, um die type-2-Header-Antwort zu sehen).
# Wer/Was/Wann/Warum: Martin.Lonkwitz / usbmon-Capture-Helfer / 2026-09-07 /
#   Referenz-Update-Format reversen (Projekt Ampersand).
# Nutzung:  sudo perl tools/qc_usb_capture.pl [sekunden] [out.pcap]
#   Laedt usbmon, findet den QC-Bus (152a:880a), schneidet auf usbmonNu mit.
# Abhaengig: usbmon, dumpcap/tshark, root.
use strict; use warnings;
my $secs = $ARGV[0] // 0;                 # 0 = bis Ctrl-C
my $out  = $ARGV[1] // 'qc-usbcap.pcap';
system('modprobe usbmon') == 0 or die "modprobe usbmon failed\n";
system('mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug');
# QC-Bus finden
my $bus;
for my $d (glob '/sys/bus/usb/devices/*') {
    next unless -f "$d/idVendor";
    open my $v, '<', "$d/idVendor" or next; my $vid = <$v>; chomp $vid; close $v;
    next unless $vid eq '152a';
    open my $b, '<', "$d/busnum" or next; $bus = <$b>; chomp $bus; close $b; last;
}
die "Quad Cortex (152a) nicht am USB gefunden - erst anstecken\n" unless defined $bus;
my $iface = "usbmon${bus}";              # dumpcap-Interface fuer diesen Bus
print "QC auf Bus $bus -> capture auf $iface -> $out\n";
my @cmd = ('dumpcap','-i',$iface,'-w',$out);
push @cmd, ('-a',"duration:$secs") if $secs > 0;
print "Kommando: @cmd\n(Ctrl-C beendet)\n";
exec @cmd;
