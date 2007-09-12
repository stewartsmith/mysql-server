/*
 * Generated by dtrace(1M).
 */

#ifndef	_PROBES_H
#define	_PROBES_H

#include <unistd.h>

#ifdef	__cplusplus
extern "C" {
#endif

#if _DTRACE_VERSION

#define	MYSQL_DELETE_END(arg0) \
	__dtrace_mysql___delete_end(arg0)
#define	MYSQL_DELETE_END_ENABLED() \
	__dtraceenabled_mysql___delete_end()
#define	MYSQL_DELETE_START(arg0) \
	__dtrace_mysql___delete_start(arg0)
#define	MYSQL_DELETE_START_ENABLED() \
	__dtraceenabled_mysql___delete_start()
#define	MYSQL_EXTERNAL_LOCK(arg0, arg1) \
	__dtrace_mysql___external_lock(arg0, arg1)
#define	MYSQL_EXTERNAL_LOCK_ENABLED() \
	__dtraceenabled_mysql___external_lock()
#define	MYSQL_FILESORT_END(arg0) \
	__dtrace_mysql___filesort_end(arg0)
#define	MYSQL_FILESORT_END_ENABLED() \
	__dtraceenabled_mysql___filesort_end()
#define	MYSQL_FILESORT_START(arg0) \
	__dtrace_mysql___filesort_start(arg0)
#define	MYSQL_FILESORT_START_ENABLED() \
	__dtraceenabled_mysql___filesort_start()
#define	MYSQL_INSERT_END(arg0) \
	__dtrace_mysql___insert_end(arg0)
#define	MYSQL_INSERT_END_ENABLED() \
	__dtraceenabled_mysql___insert_end()
#define	MYSQL_INSERT_ROW_END(arg0) \
	__dtrace_mysql___insert_row_end(arg0)
#define	MYSQL_INSERT_ROW_END_ENABLED() \
	__dtraceenabled_mysql___insert_row_end()
#define	MYSQL_INSERT_ROW_START(arg0) \
	__dtrace_mysql___insert_row_start(arg0)
#define	MYSQL_INSERT_ROW_START_ENABLED() \
	__dtraceenabled_mysql___insert_row_start()
#define	MYSQL_INSERT_START(arg0) \
	__dtrace_mysql___insert_start(arg0)
#define	MYSQL_INSERT_START_ENABLED() \
	__dtraceenabled_mysql___insert_start()
#define	MYSQL_SELECT_END(arg0) \
	__dtrace_mysql___select_end(arg0)
#define	MYSQL_SELECT_END_ENABLED() \
	__dtraceenabled_mysql___select_end()
#define	MYSQL_SELECT_START(arg0) \
	__dtrace_mysql___select_start(arg0)
#define	MYSQL_SELECT_START_ENABLED() \
	__dtraceenabled_mysql___select_start()
#define	MYSQL_UPDATE_END(arg0) \
	__dtrace_mysql___update_end(arg0)
#define	MYSQL_UPDATE_END_ENABLED() \
	__dtraceenabled_mysql___update_end()
#define	MYSQL_UPDATE_START(arg0) \
	__dtrace_mysql___update_start(arg0)
#define	MYSQL_UPDATE_START_ENABLED() \
	__dtraceenabled_mysql___update_start()


extern void __dtrace_mysql___delete_end(unsigned long);
extern int __dtraceenabled_mysql___delete_end(void);
extern void __dtrace_mysql___delete_start(unsigned long);
extern int __dtraceenabled_mysql___delete_start(void);
extern void __dtrace_mysql___external_lock(unsigned long, int);
extern int __dtraceenabled_mysql___external_lock(void);
extern void __dtrace_mysql___filesort_end(unsigned long);
extern int __dtraceenabled_mysql___filesort_end(void);
extern void __dtrace_mysql___filesort_start(unsigned long);
extern int __dtraceenabled_mysql___filesort_start(void);
extern void __dtrace_mysql___insert_end(unsigned long);
extern int __dtraceenabled_mysql___insert_end(void);
extern void __dtrace_mysql___insert_row_end(unsigned long);
extern int __dtraceenabled_mysql___insert_row_end(void);
extern void __dtrace_mysql___insert_row_start(unsigned long);
extern int __dtraceenabled_mysql___insert_row_start(void);
extern void __dtrace_mysql___insert_start(unsigned long);
extern int __dtraceenabled_mysql___insert_start(void);
extern void __dtrace_mysql___select_end(unsigned long);
extern int __dtraceenabled_mysql___select_end(void);
extern void __dtrace_mysql___select_start(unsigned long);
extern int __dtraceenabled_mysql___select_start(void);
extern void __dtrace_mysql___update_end(unsigned long);
extern int __dtraceenabled_mysql___update_end(void);
extern void __dtrace_mysql___update_start(unsigned long);
extern int __dtraceenabled_mysql___update_start(void);

#else

#define	MYSQL_DELETE_END(arg0)
#define	MYSQL_DELETE_END_ENABLED() (0)
#define	MYSQL_DELETE_START(arg0)
#define	MYSQL_DELETE_START_ENABLED() (0)
#define	MYSQL_EXTERNAL_LOCK(arg0, arg1)
#define	MYSQL_EXTERNAL_LOCK_ENABLED() (0)
#define	MYSQL_FILESORT_END(arg0)
#define	MYSQL_FILESORT_END_ENABLED() (0)
#define	MYSQL_FILESORT_START(arg0)
#define	MYSQL_FILESORT_START_ENABLED() (0)
#define	MYSQL_INSERT_END(arg0)
#define	MYSQL_INSERT_END_ENABLED() (0)
#define	MYSQL_INSERT_ROW_END(arg0)
#define	MYSQL_INSERT_ROW_END_ENABLED() (0)
#define	MYSQL_INSERT_ROW_START(arg0)
#define	MYSQL_INSERT_ROW_START_ENABLED() (0)
#define	MYSQL_INSERT_START(arg0)
#define	MYSQL_INSERT_START_ENABLED() (0)
#define	MYSQL_SELECT_END(arg0)
#define	MYSQL_SELECT_END_ENABLED() (0)
#define	MYSQL_SELECT_START(arg0)
#define	MYSQL_SELECT_START_ENABLED() (0)
#define	MYSQL_UPDATE_END(arg0)
#define	MYSQL_UPDATE_END_ENABLED() (0)
#define	MYSQL_UPDATE_START(arg0)
#define	MYSQL_UPDATE_START_ENABLED() (0)

#endif


#ifdef	__cplusplus
}
#endif

#endif	/* _PROBES_H */
