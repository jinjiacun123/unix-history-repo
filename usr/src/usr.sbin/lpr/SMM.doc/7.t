.\" Copyright (c) 1983 The Regents of the University of California.
.\" All rights reserved.
.\"
.\" Redistribution and use in source and binary forms are permitted
.\" provided that the above copyright notice and this paragraph are
.\" duplicated in all such forms and that any documentation,
.\" advertising materials, and other materials related to such
.\" distribution and use acknowledge that the software was developed
.\" by the University of California, Berkeley.  The name of the
.\" University may not be used to endorse or promote products derived
.\" from this software without specific prior written permission.
.\" THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
.\" IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
.\" WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
.\"
.\"	@(#)7.t	6.5 (Berkeley) %G%
.\"
.NH 1
Troubleshooting
.PP
There are several messages that may be generated by the
the line printer system.  This section
categorizes the most common and explains the cause
for their generation.  Where the message implies a failure,
directions are given to remedy the problem.
.PP
In the examples below, the name
.I printer
is the name of the printer from the
.I printcap
database.
.NH 2
LPR
.SH
lpr: \fIprinter\fP\|: unknown printer
.IP
The
.I printer
was not found in the
.I printcap
database.  Usually this is a typing mistake; however, it may indicate
a missing or incorrect entry in the /etc/printcap file.
.SH
lpr: \fIprinter\fP\|: jobs queued, but cannot start daemon.
.IP
The connection to 
.I lpd
on the local machine failed. 
This usually means the printer server started at
boot time has died or is hung.  Check the local socket
/dev/printer to be sure it still exists (if it does not exist,
there is no 
.I lpd
process running). 
Usually it is enough to get a super-user to type the following to
restart
.IR lpd .
.DS
% /usr/lib/lpd
.DE
You can also check the state of the master printer daemon with the following.
.DS
% ps l`cat /usr/spool/lpd.lock`
.DE
.IP
Another possibility is that the
.I lpr
program is not set-user-id to \fIroot\fP, set-group-id to group \fIdaemon\fP.
This can be checked with
.DS
% ls \-lg /usr/ucb/lpr
.DE
.SH
lpr: \fIprinter\fP\|: printer queue is disabled
.IP
This means the queue was turned off with
.DS
% lpc disable \fIprinter\fP
.DE
to prevent 
.I lpr
from putting files in the queue.  This is normally
done by the system manager when a printer is
going to be down for a long time.  The
printer can be turned back on by a super-user with
.IR lpc .
.NH 2
LPQ
.SH
waiting for \fIprinter\fP to become ready (offline ?)
.IP
The printer device could not be opened by the daemon. 
This can happen for several reasons,
the most common is that the printer is turned off-line.
This message can also be generated if the printer is out
of paper, the paper is jammed, etc.
The actual reason is dependent on the meaning
of error codes returned by system device driver. 
Not all printers supply enough information 
to distinguish when a printer is off-line or having
trouble (e.g. a printer connected through a serial line). 
Another possible cause of this message is
some other process, such as an output filter,
has an exclusive open on the device.  Your only recourse
here is to kill off the offending program(s) and
restart the printer with
.IR lpc .
.SH
\fIprinter\fP is ready and printing
.IP
The
.I lpq
program checks to see if a daemon process exists for
.I printer
and prints the file \fIstatus\fP located in the spooling directory.
If the daemon is hung, a super user can use
.I lpc
to abort the current daemon and start a new one.
.SH
waiting for \fIhost\fP to come up
.IP
This implies there is a daemon trying to connect to the remote
machine named
.I host
to send the files in the local queue. 
If the remote machine is up,
.I lpd
on the remote machine is probably dead or
hung and should be restarted as mentioned for
.IR lpr .
.SH
sending to \fIhost\fP
.IP
The files should be in the process of being transferred to the remote
.IR host .
If not, the local daemon should be aborted and started with
.IR lpc .
.SH
Warning: \fIprinter\fP is down
.IP
The printer has been marked as being unavailable with
.IR lpc .
.SH
Warning: no daemon present
.IP
The \fIlpd\fP process overseeing
the spooling queue, as specified in the ``lock'' file
in that directory, does not exist.  This normally occurs
only when the daemon has unexpectedly died.
The error log file for the printer and the \fIsyslogd\fP logs
should be checked for a
diagnostic from the deceased process.
To restart an \fIlpd\fP, use
.DS
% lpc restart \fIprinter\fP
.DE
.SH
no space on remote; waiting for queue to drain
.IP
This implies that there is insufficient disk space on the remote.
If the file is large enough, there will never be enough space on
the remote (even after the queue on the remote is empty). The solution here
is to move the spooling queue or make more free space on the remote.
.NH 2
LPRM
.SH
lprm: \fIprinter\fP\|: cannot restart printer daemon
.IP
This case is the same as when
.I lpr
prints that the daemon cannot be started.
.NH 2
LPD
.PP
The
.I lpd
program can log many different messages using \fIsyslogd\fP\|(8).
Most of these messages are about files that can not
be opened and usually imply that the
.I printcap
file or the protection modes of the files are
incorrect.   Files may also be inaccessible if people
manually manipulate the line printer system (i.e. they
bypass the
.I lpr
program). 
.PP
In addition to messages generated by 
.IR lpd ,
any of the filters that
.I lpd
spawns may log messages using \fIsyslogd\fP or to the error log file
(the file specified in the \fBlf\fP entry in \fIprintcap\fP\|).
.NH 2
LPC
.PP
.SH
couldn't start printer
.IP
This case is the same as when
.I lpr
reports that the daemon cannot be started.
.SH
cannot examine spool directory
.IP
Error messages beginning with ``cannot ...'' are usually because of
incorrect ownership or protection mode of the lock file, spooling
directory or the
.I lpc
program.
