#!/usr/bin/env bash
#
# Use a different Username for database and check if enforcer fails to connect

if [ -z "$HAVE_MYSQL" ]; then
	return 0
fi &&

! ods_reset_env &&

ods_setup_conf conf.xml conf-correct.xml &&

ods_reset_env &&

ods_setup_conf conf.xml conf.xml &&

! log_this ods-control-enforcer-start ods-control enforcer start &&
syslog_waitfor 10 "ods-enforcerd: .*ERROR: unable to connect to database - Access denied for user 'test999'@'localhost'" &&
! pgrep 'ods-enforcerd' >/dev/null 2>/dev/null &&
return 0

ods_kill
return 1