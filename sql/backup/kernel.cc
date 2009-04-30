/**
  @file

  @brief Implementation of the backup kernel API.

  @section s1 How to use backup kernel API to perform backup and restore operations
  
  To perform backup or restore operation an appropriate context must be created.
  This involves creating required resources and correctly setting up the server.
  When operation is completed or interrupted, the context must be destroyed and
  all preparations reversed.
  
  All this is accomplished by creating an instance of Backup_create_ctx class and
  then using its methods to perform the operation. When the instance is 
  destroyed, the required clean-up is performed.
  
  This is how backup is performed using the context object:
  @code
  {
  
   Backup_restore_ctx context(thd); // create context instance
   Backup_info *info= context.prepare_for_backup(location, 
                                                 orig_loc); // prepare for backup
  
   // select objects to backup
   info->add_all_dbs();
   or
   info->add_dbs(<list of db names>);
  
   info->close(); // indicate that selection is done
  
   context.do_backup(); // perform backup
   
   context.close(); // explicit clean-up
  
  } // if code jumps here, context destructor will do the clean-up automatically
  @endcode
  
  Similar code will be used for restore (bit simpler as we don't support 
  selective restores yet):
  @code
  {
  
   Backup_restore_ctx context(thd); // create context instance
   Restore_info *info= context.prepare_for_restore(location,
                                                   orig_loc); // prepare for restore
  
   context.do_restore(); // perform restore
   
   context.close(); // explicit clean-up
  
  } // if code jumps here, context destructor will do the clean-up automatically
  @endcode

  @todo Use internal table name representation when passing tables to
        backup/restore drivers.
  @todo Handle other types of meta-data in Backup_info methods.
  @todo Handle item dependencies when adding new items.
  @todo Handle other kinds of backup locations (far future).
*/

#include "../mysql_priv.h"
#include "../si_objects.h"

#include "backup_kernel.h"
#include "backup_info.h"
#include "restore_info.h"
#include "logger.h"
#include "stream.h"
#include "be_native.h"
#include "be_default.h"
#include "be_snapshot.h"
#include "be_nodata.h"
#include "transaction.h"


/** 
  Global Initialization for MYSQL backup system.
 
  @note This function is called in the server initialization sequence, just
  after it loads all its plugins.
 */
int backup_init()
{
  pthread_mutex_init(&Backup_restore_ctx::run_lock, MY_MUTEX_INIT_FAST);
  Backup_restore_ctx::run_lock_initialized= TRUE;
  return 0;
}

/**
  Global clean-up for MySQL backup system.
  
  @note This function is called in the server shut-down sequences, just before
  it shuts-down all its plugins.

  @note Due to way in which server's code is organized this function might be
  called and should work normally even in situation when backup_init() was not
  called at all.
 */
void backup_shutdown()
{
  if (Backup_restore_ctx::run_lock_initialized)
  {
    pthread_mutex_destroy(&Backup_restore_ctx::run_lock);
    Backup_restore_ctx::run_lock_initialized= FALSE;
  }
}

/*
  Forward declarations of functions used for sending response from BACKUP/RESTORE
  statement.
 */ 
static int send_error(Backup_restore_ctx &context, int error_code, ...);
static int send_reply(Backup_restore_ctx &context);


/**
  Call backup kernel API to execute backup related SQL statement.

  @param[in] thd        current thread object reference.
  @param[in] lex        results of parsing the statement.
  @param[in] backupdir  value of the backupdir variable from server.
  @param[in] overwrite  whether or not restore should overwrite existing
                        DB with same name as in backup image
  @param[in] skip_gap_event  whether or not restore should skip writing
                             the gap event if run on a master in an active
                             replication scenario

  @note This function sends response to the client (ok, result set or error).

  @note Both BACKUP and RESTORE should perform implicit commit at the beginning
  and at the end of execution. This is done by the parser after marking these
  commands with appropriate flags in @c sql_command_flags[] in sql_parse.cc.

  @returns 0 on success, error code otherwise.
 */

int
execute_backup_command(THD *thd, 
                       LEX *lex, 
                       String *backupdir, 
                       bool overwrite,
                       bool skip_gap_event)
{
  int res= 0;
  
  DBUG_ENTER("execute_backup_command");
  DBUG_ASSERT(thd && lex);
  DEBUG_SYNC(thd, "before_backup_command");

    
  using namespace backup;

  Backup_restore_ctx context(thd); // reports errors
  
  if (!context.is_valid())
    DBUG_RETURN(send_error(context, ER_BACKUP_CONTEXT_CREATE));

  /*
    Check backupdir for validity. This is needed since we cannot trust
    that the path is still valid. Access could have changed or the
    folders in the path could have been moved, deleted, etc.
  */
  if (backupdir->length() && my_access(backupdir->c_ptr(), (F_OK|W_OK)))
    DBUG_RETURN(send_error(context, ER_BACKUP_BACKUPDIR, backupdir->c_ptr()));
 
  switch (lex->sql_command) {

  case SQLCOM_BACKUP:
  {
    // prepare for backup operation
    
    DEBUG_SYNC(thd, "before_backup_prepare");
    Backup_info *info= context.prepare_for_backup(backupdir, lex->backup_dir, 
                                                  thd->query,
                                                  lex->backup_compression);
                                                              // reports errors

    if (!info || !info->is_valid())
      DBUG_RETURN(send_error(context, ER_BACKUP_BACKUP_PREPARE));

    DEBUG_SYNC(thd, "after_backup_start_backup");

    // select objects to backup

    if (lex->db_list.is_empty())
      res= info->add_all_dbs(); // backup all databases
    else
    {
      /* Backup databases specified by user. */
      res= info->add_dbs(thd, lex->db_list);
    }

    info->close(); // close catalogue after filling it with objects to backup

    if (res || !info->is_valid())
      DBUG_RETURN(send_error(context, ER_BACKUP_BACKUP_PREPARE));

    if (info->db_count() == 0)
    {
      context.report_error(ER_BACKUP_NOTHING_TO_BACKUP);
      DBUG_RETURN(send_error(context, ER_BACKUP_NOTHING_TO_BACKUP));
    }

    // perform backup

    DEBUG_SYNC(thd, "before_do_backup");
    res= context.do_backup();
 
    DEBUG_SYNC(thd, "after_do_backup");
    if (res)
      DBUG_RETURN(send_error(context, ER_BACKUP_BACKUP));

    break;
  }

  case SQLCOM_RESTORE:
  {
    DEBUG_SYNC(thd, "before_restore_prepare");

    Restore_info *info= context.prepare_for_restore(backupdir, lex->backup_dir, 
                                                    thd->query, skip_gap_event);
    
    if (!info || !info->is_valid())
      DBUG_RETURN(send_error(context, ER_BACKUP_RESTORE_PREPARE));
    
    DEBUG_SYNC(thd, "after_backup_start_restore");

    res= context.do_restore(overwrite);      

    DEBUG_SYNC(thd, "restore_before_end");

    if (res)
      DBUG_RETURN(send_error(context, ER_BACKUP_RESTORE));
    
    break;
  }

  default:
     /*
       execute_backup_command() should be called with correct command id
       from the parser. If not, we fail on this assertion.
      */
     DBUG_ASSERT(FALSE);

  } // switch(lex->sql_command)

  res= context.close();
  DEBUG_SYNC(thd, "backup_restore_done");

  if (res)
    DBUG_RETURN(send_error(context, ER_BACKUP_CONTEXT_REMOVE));

  // All seems OK - send positive reply to client

  DBUG_RETURN(send_reply(context));
}

/**
  Sends error notification after failed backup/restore operation.

  @param[in]  ctx  The context of the backup/restore operation.
  @param[in]  error_code  Error to be reported if no errors reported yet.

  If an error has been already reported, or process has been interrupted,
  then nothing is done - the first logged error or kill message will be sent 
  to the client. Otherwise, if no errors were reported yet and no interruption
  has been detected the given error is sent to the client (but not reported).
  
  @returns The error code given as argument.
*/
static
int send_error(Backup_restore_ctx &context, int error_code, ...)
{
  if (!context.is_killed() && !context.error_reported())
  {
    char buf[MYSQL_ERRMSG_SIZE];
    va_list args;
    va_start(args, error_code);

    my_vsnprintf(buf, sizeof(buf), ER_SAFE(error_code), args);
    my_printf_error(error_code, buf, MYF(0));

    va_end(args);
  }

  return error_code;
}


/**
  Send positive reply after a backup/restore operation.

  Currently the id of the operation is returned to the client. It can
  be used to select correct entries from the backup progress tables.

  @note If an error has been reported, send_error() is invoked instead.

  @returns 0 on success, error code otherwise.
*/
int send_reply(Backup_restore_ctx &context)
{
  Protocol *protocol= context.thd()->protocol;    // client comms
  List<Item> field_list;                // list of fields to send
  char buf[255];                        // buffer for llstr

  DBUG_ENTER("send_reply");

  if (context.error_reported())
    return send_error(context, ER_UNKNOWN_ERROR);

  /*
    Send field list.
  */
  if (field_list.push_back(new Item_empty_string(STRING_WITH_LEN("backup_id"))))
  {
    goto err;
  }
  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
  {
    goto err;
  }

  /*
    Send field data.
  */
  protocol->prepare_for_resend();               // Never errors
  llstr(context.op_id(), buf);                  // Never errors
  if (protocol->store(buf, system_charset_info))
  {
    goto err;
  }
  if (protocol->write())
  {
    goto err;
  }
  my_eof(context.thd());                        // Never errors
  DBUG_RETURN(0);

 err:
  DBUG_RETURN(context.report_error(ER_BACKUP_SEND_REPLY,
                                   context.m_type == backup::Logger::BACKUP
                                   ? "BACKUP" : "RESTORE"));
}


namespace backup {

/**
  This class provides memory allocation services for backup stream library.

  An instance of this class is created during preparations for backup/restore
  operation. When it is deleted, all allocated memory is freed.
*/
class Mem_allocator
{
public:

  Mem_allocator();
  ~Mem_allocator();

  void* alloc(size_t);
  void  free(void*);

private:

  struct node;
  node *first;  ///< Pointer to the first segment in the list.
};


} // backup namespace


/*************************************************

   Implementation of Backup_restore_ctx class

 *************************************************/

// static members

Backup_restore_ctx *Backup_restore_ctx::current_op= NULL;
bool Backup_restore_ctx::run_lock_initialized= FALSE;
pthread_mutex_t Backup_restore_ctx::run_lock;


Backup_restore_ctx::Backup_restore_ctx(THD *thd)
 :Logger(thd), m_state(CREATED), m_thd_options(thd->options),
  m_error(0), m_stream(NULL),
  m_catalog(NULL), mem_alloc(NULL), m_tables_locked(FALSE),
  m_engage_binlog(FALSE), m_completed(FALSE)
{
  /*
    Check for progress tables.
  */
  MYSQL_BACKUP_LOG *backup_log= logger.get_backup_history_log_file_handler();
  if (report_killed())
    m_error= ER_QUERY_INTERRUPTED;
  else if (backup_log->check_backup_logs(thd))
    m_error= ER_BACKUP_PROGRESS_TABLES;
}

Backup_restore_ctx::~Backup_restore_ctx()
{
  DEBUG_SYNC(m_thd, "backup_restore_ctx_dtor");
  close();

  delete mem_alloc;
  delete m_catalog;  
  delete m_stream;
}

/**
  Prepare path for access.

  This method takes the backupdir and the file name specified on the backup
  command (orig_loc) and forms a combined path + file name as follows:

    1. If orig_loc has a relative path, make it relative to backupdir
    2. If orig_loc has a hard path, use it.
    3. If orig_loc has no path, append to backupdir

  @param[in]  backupdir  The backupdir system variable value.
  @param[in]  orig_loc   The path + file name specified in the backup command.

  @returns 0
*/
int Backup_restore_ctx::prepare_path(::String *backupdir, 
                                     LEX_STRING orig_loc)
{
  char fix_path[FN_REFLEN]; 
  char full_path[FN_REFLEN]; 

  /*
    Prepare the path using the backupdir iff no relative path
    or no hard path included.

    Relative paths are formed from the backupdir system variable.

    Case 1: Backup image file name has relative path. 
            Make relative to backupdir.

    Example BACKUP DATATBASE ... TO '../monthly/dec.bak'
            If backupdir = '/dev/daily' then the
            calculated path becomes
            '/dev/monthly/dec.bak'

    Case 2: Backup image file name has no path or has a subpath. 

    Example BACKUP DATABASE ... TO 'week2.bak'
            If backupdir = '/dev/weekly/' then the
            calculated path becomes
            '/dev/weekly/week2.bak'
    Example BACKUP DATABASE ... TO 'jan/day1.bak'
            If backupdir = '/dev/monthly/' then the
            calculated path becomes
            '/dev/monthly/jan/day1.bak'

    Case 3: Backup image file name has hard path. 

    Example BACKUP DATATBASE ... TO '/dev/dec.bak'
            If backupdir = '/dev/daily/backup' then the
            calculated path becomes
            '/dev/dec.bak'
  */

  /*
    First, we construct the complete path from backupdir.
  */
  fn_format(fix_path, backupdir->ptr(), mysql_real_data_home, "", 
            MY_UNPACK_FILENAME | MY_RELATIVE_PATH);

  /*
    Next, we contruct the full path to the backup file.
  */
  fn_format(full_path, orig_loc.str, fix_path, "", 
            MY_UNPACK_FILENAME | MY_RELATIVE_PATH);

  /*
    Copy result to member variable for Stream class.
  */
  m_path.copy(full_path, strlen(full_path), system_charset_info);
  report_backup_file(m_path.c_ptr());
 
  return 0;
}

/**
  Do preparations common to backup and restore operations.
  
  It is checked if another operation is in progress and if yes then
  error is reported. Otherwise the current operation is registered so that
  no other can be started. All preparations common to backup and restore 
  operations are done. In particular, all changes to meta data are blocked
  with DDL blocker.

  @returns 0 on success, error code otherwise.
 */ 
int Backup_restore_ctx::prepare(::String *backupdir, LEX_STRING location)
{
  if (m_error)
    return m_error;

  int ret= 0;

  /*
    Check access for SUPER rights. If user does not have SUPER, fail with error.

    In case of error, we write only to backup logs, because check_global_access()
    pushes the same error on the error stack.
  */
  DEBUG_SYNC(m_thd, "before_backup_privileges");
  ret= check_global_access(m_thd, SUPER_ACL);
  if (ret || is_killed())
    return fatal_error(log_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, "SUPER"));

  /*
    Check if another BACKUP/RESTORE is running and if not, register 
    this operation.
   */

  DEBUG_SYNC(m_thd, "before_backup_single_op");
  pthread_mutex_lock(&run_lock);

  if (!current_op)
    current_op= this;
  else
    fatal_error(ER_BACKUP_RUNNING);

  pthread_mutex_unlock(&run_lock);

  if (m_error)
  {
    report_error(ER_BACKUP_RUNNING);
    return m_error;
  }

  // check if location is valid (we assume it is a file path)

  /*
    For this error to work correctly, we need to check original
    file specified by the user rather than the path formed
    using the backupdir.
  */
  bool bad_filename= (location.length == 0);
  
  /*
    On some systems certain file names are invalid. We use 
    check_if_legal_filename() function from mysys to detect this.
   */ 
#if defined(__WIN__) || defined(__EMX__)  

  bad_filename = bad_filename || check_if_legal_filename(location.str);

#endif

  if (bad_filename || is_killed())
    return fatal_error(report_error(ER_BAD_PATH, location.str));

  /*
    Computer full path to backup file.
  */
  prepare_path(backupdir, location);

  // create new instance of memory allocator for backup stream library

  using namespace backup;

  delete mem_alloc;
  mem_alloc= new Mem_allocator();

  if (!mem_alloc)
    return fatal_error(report_error(ER_OUT_OF_RESOURCES));

  // Freeze all meta-data. 

  DEBUG_SYNC(m_thd, "before_backup_ddl_block");
  ret= obs::bml_get(m_thd);
  if (ret || is_killed())
    return fatal_error(report_error(ER_DDL_BLOCK));

  DEBUG_SYNC(m_thd, "after_backup_restore_prepare");

  return 0;
}

/**
  Prepare for backup operation.
  
  @param[in] backupdir  path to the file where backup image should be stored
  @param[in] orig_loc   path as specified on command line for backup image
  @param[in] query      BACKUP query starting the operation
  @param[in] with_compression  backup image compression switch
  
  @returns Pointer to a @c Backup_info instance which can be used for selecting
  which objects to backup. NULL if an error was detected.
  
  @note This function reports errors.

  @note It is important that changes of meta-data are blocked as part of the
  preparations. The set of server objects and their definitions should not
  change after the backup context has been prepared and before the actual backup
  is performed using @c do_backup() method.
 */ 
Backup_info* 
Backup_restore_ctx::prepare_for_backup(String *backupdir, 
                                       LEX_STRING orig_loc, 
                                       const char *query,
                                       bool with_compression)
{
  using namespace backup;
  
  // Do nothing if context is in error state.
  if (m_error)
    return NULL;

  /*
   Note: Logger must be initialized before any call to report_error() - 
   otherwise an assertion will fail.
  */ 
  DEBUG_SYNC(m_thd, "before_backup_logger_init");
  int ret= Logger::init(BACKUP, query);  // Logs errors   
  if (ret || is_killed())
  {
    fatal_error(report_killed() ? ER_QUERY_INTERRUPTED : ER_BACKUP_LOGGER_INIT);
    return NULL;
  }

  time_t when= my_time(0);
  report_start(when);
  
  /*
    Do preparations common to backup and restore operations. After call
    to prepare() all meta-data changes are blocked.
   */ 
  DEBUG_SYNC(m_thd, "before_backup_common_prepare");
  if (prepare(backupdir, orig_loc))  // Logs errors and detects interruptions
    return NULL;

  /*
    Open output stream.
   */
  DEBUG_SYNC(m_thd, "before_backup_open_stream");
  Output_stream *s= new Output_stream(*this, 
                                      &m_path,
                                      with_compression);
  m_stream= s;

  if (!s || is_killed())
  {
    fatal_error(report_error(ER_OUT_OF_RESOURCES));
    return NULL;
  }

  DEBUG_SYNC(m_thd, "before_backup_stream_open");
  int my_open_status= s->open();

  /*
    Set state to PREPARED_FOR_BACKUP in case output file was opened successfuly.
    It is important to set the state here, because this ensures that the file
    will be removed in case of error or interruption.
   */ 
  if (! my_open_status)
    m_state= PREPARED_FOR_BACKUP;
  
  if (my_open_status != 0 || is_killed())
  {
    if (report_killed())
      fatal_error(ER_QUERY_INTERRUPTED);
    else
      report_stream_open_failure(my_open_status, &orig_loc);
    return NULL;
  }

  /*
    Create backup catalogue.
   */

  DEBUG_SYNC(m_thd, "before_backup_catalog");
  Backup_info *info= new Backup_info(*this, m_thd);    // Logs errors
  m_catalog= info;

  if (!info || is_killed())
  {
    fatal_error(report_error(ER_OUT_OF_RESOURCES));
    return NULL;
  }
  if (!info->is_valid())
  {
    // Error has been logged by Backup_info constructor
    fatal_error(ER_BACKUP_BACKUP_PREPARE);
    return NULL;    
  }

  /*
    If binlog is enabled, set BSTREAM_FLAG_BINLOG in the header to indicate
    that validity point's binlog position will be stored in the image 
    (in its summary section).
    
    This is not completely safe because theoretically even if now binlog is 
    active, it can be disabled before we reach the validity point and then we 
    will not store binlog position even though the flag is set. To fix this 
    problem the format of backup image must be changed (some flags must be 
    stored in the summary section which is written at the end of backup 
    operation).
  */
  if (mysql_bin_log.is_open())
    info->flags|= BSTREAM_FLAG_BINLOG; 

  info->save_start_time(when);

  return info;
}

/**
  Prepare for restore operation.
  
  @param[in] backupdir  path to the file where backup image is stored
  @param[in] orig_loc   path as specified on command line for backup image
  @param[in] query      RESTORE query starting the operation
  @param[in] skip_gap_event TRUE means do not write gap event
  
  @returns Pointer to a @c Restore_info instance containing catalogue of the
  backup image (read from the image). NULL if errors were detected.
  
  @note This function reports errors.
 */ 
Restore_info* 
Backup_restore_ctx::prepare_for_restore(String *backupdir,
                                        LEX_STRING orig_loc, 
                                        const char *query,
                                        bool skip_gap_event)
{
  using namespace backup;  


  // Do nothing if context is in error state.
  if (m_error)
    return NULL;
  
  /*
   Note: Logger must be initialized before any call to report_error() - 
   otherwise an assertion will fail.
  */ 
  DEBUG_SYNC(m_thd, "before_restore_logger_init");
  int ret= Logger::init(RESTORE, query);  // Logs errors
  if (ret || is_killed())
  {
    fatal_error(ER_BACKUP_LOGGER_INIT);
    return NULL;
  }

  /*
    Block replication from starting.
  */
  obs::block_replication(TRUE, "RESTORE");

  /*
    Restore cannot be run on a slave while connected to a master.
  */
  if (obs::is_slave())
  {
    fatal_error(report_error(ER_RESTORE_ON_SLAVE));
    return NULL;
  }

  time_t when= my_time(0);  
  report_start(when);

  /*
    Do preparations common to backup and restore operations. After this call
    changes of meta-data are blocked.
   */ 
  DEBUG_SYNC(m_thd, "before_restore_common_prepare");
  if (prepare(backupdir, orig_loc))
    return NULL;
  
  /*
    Open input stream.
   */
  DEBUG_SYNC(m_thd, "before_restore_open_stream");
  Input_stream *s= new Input_stream(*this, &m_path);
  m_stream= s;
  
  if (!s || is_killed())
  {
    fatal_error(report_error(ER_OUT_OF_RESOURCES));
    return NULL;
  }
  
  DEBUG_SYNC(m_thd, "before_restore_stream_open");
  int my_open_status= s->open();
  if (my_open_status != 0 || is_killed())
  {
    if (report_killed())
      fatal_error(ER_QUERY_INTERRUPTED);
    else
      report_stream_open_failure(my_open_status, &orig_loc);
    return NULL;
  }

  /*
    Create restore catalogue.
   */

  DEBUG_SYNC(m_thd, "before_restore_catalog");
  Restore_info *info= new Restore_info(*this, m_thd);  // reports errors
  m_catalog= info;

  if (!info || is_killed())
  {
    fatal_error(report_error(ER_OUT_OF_RESOURCES));
    return NULL;
  }
  if (!info->is_valid())
  {
    // Errors are logged by Restore_info constructor. 
    fatal_error(ER_BACKUP_RESTORE_PREPARE); 
    return NULL;
  }

  info->save_start_time(when);

  /*
    Read header and catalogue from the input stream.
   */

  DEBUG_SYNC(m_thd, "before_restore_read_header");
  ret= read_header(*info, *s);  // Can log errors via callback functions.
  if (ret || is_killed())
  {
    if (report_killed())
      ret= ER_QUERY_INTERRUPTED;
    else if (!error_reported())
      report_error(ER_BACKUP_READ_HEADER);
    fatal_error(ret);
    return NULL;
  }

  ret= s->next_chunk();
  /* Mimic error in next_chunk */
  DBUG_EXECUTE_IF("restore_prepare_next_chunk_1", ret= BSTREAM_ERROR; );
  if (ret != BSTREAM_OK || is_killed())
  {
    fatal_error(report_error(ER_BACKUP_NEXT_CHUNK));
    return NULL;
  }

  DEBUG_SYNC(m_thd, "before_restore_read_catalog");
  ret= read_catalog(*info, *s);  // Can log errors via callback functions.
  if (ret || is_killed())
  {
    if (report_killed())
      ret= ER_QUERY_INTERRUPTED;
    else if (!error_reported())
      report_error(ER_BACKUP_READ_HEADER);
    fatal_error(ret);
    return NULL;
  }

  ret= s->next_chunk();
  /* Mimic error in next_chunk */
  DBUG_EXECUTE_IF("restore_prepare_next_chunk_2", ret= BSTREAM_ERROR; );
  if (ret != BSTREAM_OK || is_killed())
  {
    fatal_error(report_error(ER_BACKUP_NEXT_CHUNK));
    return NULL;
  }

  m_state= PREPARED_FOR_RESTORE;

  DEBUG_SYNC(m_thd, "before_restore_binlog");
  /*
    Do not allow slaves to connect during a restore.

    If the binlog is turned on, write a RESTORE_EVENT as an
    incident report into the binary log.

    Turn off binlog during restore.
  */
  if (obs::is_binlog_engaged())
  {
    obs::disable_slave_connections(TRUE);

    DEBUG_SYNC(m_thd, "after_disable_slave_connections");

    if (!skip_gap_event)
      obs::write_incident_event(m_thd, obs::RESTORE_EVENT);
    m_engage_binlog= TRUE;
    obs::engage_binlog(FALSE);
  }

  if (report_killed())
  {
    fatal_error(ER_QUERY_INTERRUPTED);
    return NULL;
  }
  return info;
}

/*
  Lock tables being restored.

  Backup kernel ensures that all tables being restored are exclusively locked.

  We use open_and_lock_tables() for locking. This is a temporary solution until
  a better mechanism is devised - open_and_lock_tables() is not good if there
  are many tables to be processed.

  The built-in restore drivers need to open tables to write rows to them. Since
  we have opened tables here, we store pointers to opened TABLE_LIST structures
  in the restore catalogue so that the built-in drivers can access them later.

  @todo Replace open_and_lock_tables() by a lighter solution.
  @todo Hide table locking behind the server API.
*/ 
int Backup_restore_ctx::lock_tables_for_restore()
{
  int ret;

  /*
    Iterate over all tables in all snapshots and create a linked
    TABLE_LIST for call to open_and_lock_tables(). Remember the
    TABLE_LIST for later use in Backup_restore_ctx::unlock_tables().
    Store pointers to TABLE_LIST structures in the restore catalogue for
    later access to opened tables.
  */

  m_backup_tables= NULL;
  for (uint s= 0; s < m_catalog->snap_count(); ++s)
  {
    backup::Snapshot_info *snap= m_catalog->m_snap[s];

    for (ulong t=0; t < snap->table_count(); ++t)
    {
      backup::Image_info::Table *tbl= snap->get_table(t);
      DBUG_ASSERT(tbl); // All tables should be present in the catalogue.

      TABLE_LIST *ptr= backup::mk_table_list(*tbl, TL_WRITE, m_thd->mem_root);
      if (!ptr)
      {
        // Error has been reported, but not logged to backup logs
        return fatal_error(log_error(ER_OUT_OF_RESOURCES));
      }

      m_backup_tables= backup::link_table_list(*ptr, m_backup_tables);
      DBUG_ASSERT(m_backup_tables);
      tbl->m_table= ptr;
    }
  }

  DBUG_EXECUTE_IF("restore_lock_tables_for_restore",
    /* 
       Mimic error in opening tables. Cannot be done by setting ret=1
       after open_and_lock_tables_derived becase that method is
       supposed to release the lock before returning error.
     */
    return fatal_error(report_error(ER_BACKUP_OPEN_TABLES,"RESTORE"));
  );

  /*
    Open and lock the tables.
    
    Note 1: It is important to not do derived tables processing here. Processing
    derived tables even leads to crashes as those reported in BUG#34758.
  
    Note 2: Skiping tmp tables is also important because otherwise a tmp table
    can occlude a regular table with the same name (BUG#33574).
  */ 
  ret= open_and_lock_tables_derived(m_thd, m_backup_tables,
                                    FALSE, /* do not process derived tables */
                                    MYSQL_OPEN_SKIP_TEMPORARY 
                                          /* do not open tmp tables */
                                   );
  if (ret || is_killed())
    return fatal_error(report_error(ER_BACKUP_OPEN_TABLES,"RESTORE"));

  m_tables_locked= TRUE;
  return 0;
}

/**
  Unlock tables which were locked by @c lock_tables_for_restore.
 */ 
void Backup_restore_ctx::unlock_tables()
{
  // Do nothing if tables are not locked.
  if (!m_tables_locked)
    return;

  DBUG_PRINT("restore",("unlocking tables"));

  /*
    Refresh tables that have been restored. Some restore drivers might
    restore a table layout that differs from the version created by
    materialize(). We need to force a final close after restore with
    close_cached_tables(). Note that we do this before we unlock the
    tables. Otherwise other threads could use the still open tables
    before we refresh them.

    For information about a concrete problem, see the comment in
    myisam_backup_engine.cc:Table_restore::close().

    Use the restore table list as created by lock_tables_for_restore().
  */
  if (m_backup_tables)
    close_cached_tables(m_thd, m_backup_tables, FALSE, FALSE);

  close_thread_tables(m_thd);                   // Never errors
  m_tables_locked= FALSE;

  return;
}


/**
  Destroy a backup/restore context.
  
  This should reverse all settings made when context was created and prepared.
  If it was requested, the backup/restore location is removed. Also, the backup
  stream memory allocator is shut down. Any other allocated resources are 
  deleted in the destructor. Changes to meta-data are unblocked.
  
  @returns 0 or error code if error was detected.
  
  @note This function reports errors.
 */ 
int Backup_restore_ctx::close()
{
  if (m_state == CLOSED)
    return 0;

  /*
    Note: The standard report_error() method does not log an error in case
    the statement has been interrupted - the interruption is reported instead.
    But we want to always report errors which happen during shutdown phase.
    Therefore, for reporting errors here we use the variant of 
    Logger::report_error() with explicit log_level::ERROR. This variant does not 
    check for interruptions and always reports given error.
  */ 

  using namespace backup;

  // Move context to error state if the catalog became corrupted.
  if (m_catalog && !m_catalog->is_valid())
    fatal_error(m_type == BACKUP ? ER_BACKUP_BACKUP : ER_BACKUP_RESTORE);

  /*
    Report end of the operation which has started if it has not been done 
    before (Logger is in RUNNING state). 
  */ 
  if (Logger::m_state == RUNNING)
  {
    time_t  now= my_time(0);
    if (m_catalog)
      m_catalog->save_end_time(now);

    // Report either completion or interruption depending on m_completed flag.
    if (m_completed)
      report_completed(now);
    else
    {
      /*
        If this is restore operation then m_data_changed flag in the 
        Restore_info object tells if data has been modified or not.
       */ 
      const bool data_changed= m_type==RESTORE && m_catalog && 
                         static_cast<Restore_info*>(m_catalog)->m_data_changed;
      report_aborted(now, data_changed);
    }
  }

  /*
    Allow slaves connect after restore is complete.
  */
  obs::disable_slave_connections(FALSE);

  /*
    Allow replication to start after restore is complete.
  */
  obs::block_replication(FALSE, "");

  /*
    Turn binlog back on iff it was turned off earlier.
  */
  if (m_engage_binlog)
    obs::engage_binlog(TRUE);

  // unlock tables if they are still locked
  unlock_tables();                              // Never errors

  // unfreeze meta-data
  obs::bml_release();                           // Never errors

  // restore thread options

  m_thd->options= m_thd_options;

  // close stream if not closed already (in which case m_steam is NULL)

  if (m_stream && !m_stream->close())
  {
    // Note error, but complete clean-up
    fatal_error(report_error(log_level::ERROR, ER_BACKUP_CLOSE));
  }

  /* 
    Remove the location if it is BACKUP operation and it has not completed
    successfully.
    
    Important: RESTORE should never remove the specified backup image!
   */
  if (m_state == PREPARED_FOR_BACKUP && !m_completed)
  {
    int ret= my_delete(m_path.c_ptr(), MYF(0));
    DBUG_EXECUTE_IF("backup_remove_location_error", ret= TRUE;);
    /*
      Ignore ENOENT error since it is ok if the file doesn't exist.
     */
    if (ret && my_errno != ENOENT)
    {
      fatal_error(report_error(log_level::ERROR, ER_CANT_DELETE_FILE, 
                               m_path.c_ptr(), my_errno));
    }
  }

  /* 
    Destroy backup stream's memory allocator (this frees memory)
  
    Note that from now on data stored in this object might be corrupted. For 
    example the binlog file name is a string stored in memory allocated by
    the allocator which will be freed now.
  */
  
  delete mem_alloc;
  mem_alloc= NULL;
  
  // deregister this operation if it was running
  pthread_mutex_lock(&run_lock);
  if (current_op == this) {
    current_op= NULL;
  }
  pthread_mutex_unlock(&run_lock);

  m_state= CLOSED;
  return m_error;
}

/**
  Create backup archive.
  
  @pre @c prepare_for_backup() method was called.

  @returns 0 on success, error code otherwise.
*/
int Backup_restore_ctx::do_backup()
{
  DBUG_ENTER("do_backup");

  // This function should not be called when context is not valid
  DBUG_ASSERT(is_valid());
  DBUG_ASSERT(m_state == PREPARED_FOR_BACKUP);
  DBUG_ASSERT(m_thd);
  DBUG_ASSERT(m_stream);
  DBUG_ASSERT(m_catalog);
  
  using namespace backup;

  int ret;
  Output_stream &s= *static_cast<Output_stream*>(m_stream);
  Backup_info   &info= *static_cast<Backup_info*>(m_catalog);

  DEBUG_SYNC(m_thd, "before_backup_meta");

  report_stats_pre(info);                       // Never errors

  if (report_killed())
    DBUG_RETURN(fatal_error(ER_QUERY_INTERRUPTED));

  DBUG_PRINT("backup",("Writing preamble"));
  DEBUG_SYNC(m_thd, "backup_before_write_preamble");

  ret= write_preamble(info, s);  // Can Log errors via callback functions.
  if (ret || is_killed())
  {
    if (report_killed())
      ret= ER_QUERY_INTERRUPTED;
    else if (!error_reported())
      report_error(ER_BACKUP_WRITE_HEADER);
    DBUG_RETURN(fatal_error(ret));
  }

  DBUG_PRINT("backup",("Writing table data"));

  DEBUG_SYNC(m_thd, "before_backup_data");

  ret= write_table_data(m_thd, info, s); // logs errors and detects interruptions
  if (ret)
    DBUG_RETURN(fatal_error(ret));

  DBUG_PRINT("backup",("Writing summary"));

  DEBUG_SYNC(m_thd, "before_backup_summary");
  ret= write_summary(info, s);  
  if (ret || is_killed())
    DBUG_RETURN(fatal_error(report_error(ER_BACKUP_WRITE_SUMMARY)));

  DEBUG_SYNC(m_thd, "before_backup_completed");
  m_completed= TRUE;
  report_stats_post(info);                      // Never errors

  DBUG_PRINT("backup",("Backup done."));
  DEBUG_SYNC(m_thd, "before_backup_done");

  DBUG_RETURN(close());
}

/**
  Create all triggers and events from restore catalogue.

  This helper method iterates over all triggers and events stored in the 
  restore catalogue and creates them. When metadata section of the backup image 
  is read, trigger and event objects are materialized and stored in the 
  catalogue but they are not executed then (see @c bcat_create_item()). 
  This method can be used to re-create the corresponding server objects after 
  all other objects and table data have been restored.

  Note that we first restore all triggers and then the events.

  @returns 0 on success, error code otherwise.
*/ 
int Backup_restore_ctx::restore_triggers_and_events()
{
  using namespace backup;

  DBUG_ASSERT(m_catalog);

  DBUG_ENTER("restore_triggers_and_events");

  Image_info::Obj *obj;
  List<Image_info::Obj> events;
  Image_info::Obj::describe_buf buf;
  // Note: The two iterators below must be deleted upon exit.
  Image_info::Iterator *trgit= NULL;
  Image_info::Iterator *dbit= m_catalog->get_dbs();
  if (!dbit || is_killed())
  {
    fatal_error(report_error(ER_OUT_OF_RESOURCES));
    goto exit;
  }

  // create all trigers and collect events in the events list
  
  while ((obj= (*dbit)++)) 
  {
    trgit= m_catalog->get_db_objects(*static_cast<Image_info::Db*>(obj));
    if (!trgit || is_killed())
    {
      fatal_error(report_error(ER_OUT_OF_RESOURCES));
      goto exit;
    }

    while ((obj= (*trgit)++))
      switch (obj->type()) {
      
      case BSTREAM_IT_EVENT:
        DBUG_ASSERT(obj->m_obj_ptr);
        if (events.push_back(obj))
        {
          // Error has been reported, but not logged to backup logs
          fatal_error(log_error(ER_OUT_OF_RESOURCES));
          goto exit; 
        }
        break;
      
      case BSTREAM_IT_TRIGGER:
      {
        DBUG_ASSERT(obj->m_obj_ptr);
        int ret= obj->m_obj_ptr->create(m_thd);
        /* Mimic error in restore of trigger */
        DBUG_EXECUTE_IF("restore_trigger", ret= TRUE;); 
        if (ret || is_killed())
        {
          fatal_error(report_error(ER_BACKUP_CANT_RESTORE_TRIGGER,
                                   obj->describe(buf)));
          goto exit;
        }
        break;
      }
      default: break;      
      }

    delete trgit;
    trgit= NULL;
  }

  delete dbit;
  dbit= NULL;

  // now create all events
  
  {
    List_iterator<Image_info::Obj> it(events);
    Image_info::Obj *ev;

    while ((ev= it++))
    {
      int ret= ev->m_obj_ptr->create(m_thd); 
      if (ret || is_killed())
      {
        fatal_error(report_error(ER_BACKUP_CANT_RESTORE_EVENT,ev->describe(buf)));
        goto exit;
      }
    }
  }

  /* 
    FIXME: this call is here because object services doesn't clean the
    statement execution context properly, which leads to assertion failure.
    It should be fixed inside object services implementation and then the
    following line should be removed (see BUG#41294).
   */
  close_thread_tables(m_thd);                   // Never errors
  m_thd->clear_error();                         // Never errors

exit:

  delete dbit;
  delete trgit;
  DBUG_RETURN(m_error);
}

/**
  Restore objects saved in backup image.

  @pre @c prepare_for_restore() method was called.

  @param[in] overwrite whether or not restore should overwrite existing
                       DB with same name as in backup image

  @returns 0 on success, error code otherwise.

  @todo Remove the @c reset_diagnostic_area() hack.
*/
int Backup_restore_ctx::do_restore(bool overwrite)
{
  DBUG_ENTER("do_restore");

  DBUG_ASSERT(is_valid());
  DBUG_ASSERT(m_state == PREPARED_FOR_RESTORE);
  DBUG_ASSERT(m_thd);
  DBUG_ASSERT(m_stream);
  DBUG_ASSERT(m_catalog);

  using namespace backup;

  int err;
  Input_stream &s= *static_cast<Input_stream*>(m_stream);
  Restore_info &info= *static_cast<Restore_info*>(m_catalog);

  DEBUG_SYNC(m_thd, "start_do_restore");

  report_stats_pre(info);                       // Never errors

  if (report_killed())
    DBUG_RETURN(fatal_error(ER_QUERY_INTERRUPTED));

  DBUG_PRINT("restore", ("Restoring meta-data"));

  // unless RESTORE... OVERWRITE: return error if database already exists
  if (!overwrite)
  {
    Image_info::Db_iterator *dbit= info.get_dbs();

    if (!dbit)
      DBUG_RETURN(fatal_error(report_error(ER_OUT_OF_RESOURCES)));

    Image_info::Db *mydb;
    while ((mydb= static_cast<Image_info::Db*>((*dbit)++)))
    {
      err= obs::check_db_existence(m_thd, &mydb->name());
      if (!err || is_killed()) 
      {
        delete dbit;
        err= report_error(ER_RESTORE_DB_EXISTS, mydb->name().ptr());
        DBUG_RETURN(fatal_error(err));
      }
    }
    delete dbit;
  }

  DEBUG_SYNC(m_thd, "before_restore_fkey_disable");
  disable_fkey_constraints();                   // Never errors

  if (report_killed())
    DBUG_RETURN(fatal_error(ER_QUERY_INTERRUPTED));

  DEBUG_SYNC(m_thd, "before_restore_read_metadata");
  err= read_meta_data(m_thd, info, s); // Can log errors via callback functions.
  if (err || is_killed())
  {
    if (report_killed())
      err= ER_QUERY_INTERRUPTED;
    else if (!error_reported())
      report_error(ER_BACKUP_READ_META);
    DBUG_RETURN(fatal_error(err));
  }

  err= s.next_chunk();
  /* Mimic error in next_chunk */
  DBUG_EXECUTE_IF("restore_stream_next_chunk", err= BSTREAM_ERROR; );
  if (err == BSTREAM_ERROR || is_killed())
  {
    DBUG_RETURN(fatal_error(report_error(ER_BACKUP_NEXT_CHUNK)));
  }

  DBUG_PRINT("restore",("Restoring table data"));

  DEBUG_SYNC(m_thd, "before_restore_locks_tables");
  err= lock_tables_for_restore();               // logs errors
  if (err || is_killed())
    DBUG_RETURN(fatal_error(report_killed() ? ER_QUERY_INTERRUPTED : err));

  DEBUG_SYNC(m_thd, "after_restore_locks_tables");
  /* 
   Here restore drivers are created to restore table data. Data is being
   (potentially) changed so we set m_data_changed flag.
  */
  info.m_data_changed= TRUE;
  DEBUG_SYNC(m_thd, "before_restore_table_data");
  err= restore_table_data(m_thd, info, s);      // logs errors

  unlock_tables();                              // Never errors
  if (err || is_killed())
    DBUG_RETURN(fatal_error(report_killed() ? ER_QUERY_INTERRUPTED : err));

  /* 
   Re-create all triggers and events (it was not done in @c bcat_create_item()).

   Note: it is important to do that after tables are unlocked, otherwise 
   creation of these objects will fail.
  */

  DEBUG_SYNC(m_thd, "before_restore_triggers");
  err= restore_triggers_and_events();   // logs errors and detects interruptions
  if (err)
     DBUG_RETURN(fatal_error(err));

  DBUG_PRINT("restore",("Done."));

  DEBUG_SYNC(m_thd, "before_restore_completed");
  m_completed= TRUE;

  err= read_summary(info, s);
  if (err || is_killed())
    DBUG_RETURN(fatal_error(report_error(ER_BACKUP_READ_SUMMARY)));

  /*
    Report validity point time and binlog position stored in the backup image
    (in the summary section).
   */ 

  report_vp_time(info.get_vp_time(), FALSE); // FALSE = do not write to progress log
  if (info.flags & BSTREAM_FLAG_BINLOG)
    report_binlog_pos(info.binlog_pos);

  report_stats_post(info);                      // Never errors

  DEBUG_SYNC(m_thd, "before_restore_done");

  DBUG_RETURN(close());
}

/**
  Report stream open error and move context object into error state.
  
  @return error code given as input or the one stored in the context
  object if a fatal error has already been reported.
 */ 
int Backup_restore_ctx::report_stream_open_failure(int my_open_status,
                                                   const LEX_STRING *location)
{
  int error= 0;
  switch (my_open_status) {
    case ER_OPTION_PREVENTS_STATEMENT:
      error= report_error(ER_OPTION_PREVENTS_STATEMENT, "--secure-backup-file-priv");
      break;
    case ER_BACKUP_WRITE_LOC:
      /*
        For this error, use the actual value returned instead of the
        path complimented with backupdir.
      */
      error= report_error(ER_BACKUP_WRITE_LOC, location->str);
      break;
    case ER_BACKUP_READ_LOC:
      /*
        For this error, use the actual value returned instead of the
        path complimented with backupdir.
      */
      error= report_error(ER_BACKUP_READ_LOC, location->str);
      break;
    default:
      DBUG_ASSERT(FALSE);
  }
  return fatal_error(error);
}

namespace backup {

/*************************************************

    Implementation of Mem_allocator class.

 *************************************************/

/// All allocated memory segments are linked into a list using this structure.
struct Mem_allocator::node
{
  node *prev;   ///< pointer to previous node in list
  node *next;   ///< pointer to next node in the list
};

Mem_allocator::Mem_allocator() :first(NULL)
{}

/// Deletes all allocated segments which have not been freed explicitly.
Mem_allocator::~Mem_allocator()
{
  node *n= first;

  while (n)
  {
    first= n->next;
    my_free(n, MYF(0));
    n= first;
  }
}

/**
  Allocate memory segment of given size.

  Extra memory is allocated for @c node structure which holds pointers
  to previous and next segment in the segments list. This is used when
  deallocating allocated memory in the destructor.
*/
void* Mem_allocator::alloc(size_t howmuch)
{
  void *ptr= my_malloc(sizeof(node) + howmuch, MYF(0));

  if (!ptr)
    return NULL;

  node *n= (node*)ptr;
  ptr= n + 1;

  n->prev= NULL;
  n->next= first;
  if (first)
    first->prev= n;
  first= n;

  return ptr;
}

/**
  Explicit deallocation of previously allocated segment.

  The @c ptr should contain an address which was obtained from
  @c Mem_allocator::alloc().

  The deallocated fragment is removed from the allocated fragments list.
*/
void Mem_allocator::free(void *ptr)
{
  if (!ptr)
    return;

  node *n= ((node*)ptr) - 1;

  if (first == n)
    first= n->next;

  if (n->prev)
    n->prev->next= n->next;

  if (n->next)
    n->next->prev= n->prev;

  my_free(n, MYF(0));
}

} // backup namespace


/*************************************************

               CATALOGUE SERVICES

 *************************************************/

/**
  Memory allocator for backup stream library.

  @pre A backup/restore context has been created and prepared for the 
  operation (one of @c Backup_restore_ctx::prepare_for_backup() or 
  @c Backup_restore_ctx::prepare_for_restore() have been called).
 */
extern "C"
bstream_byte* bstream_alloc(unsigned long int size)
{
  using namespace backup;

  DBUG_ASSERT(Backup_restore_ctx::current_op 
              && Backup_restore_ctx::current_op->mem_alloc);

  return (bstream_byte*)Backup_restore_ctx::current_op->mem_alloc->alloc(size);
}

/**
  Memory deallocator for backup stream library.
*/
extern "C"
void bstream_free(bstream_byte *ptr)
{
  using namespace backup;
  if (Backup_restore_ctx::current_op 
      && Backup_restore_ctx::current_op->mem_alloc)
    Backup_restore_ctx::current_op->mem_alloc->free(ptr);
}

/**
  Prepare restore catalogue for populating it with items read from
  backup image.

  At this point we know the list of table data snapshots present in the image
  (it was read from image's header). Here we create @c Snapshot_info object
  for each of them.

  @param[in]  catalogue  The catalogue to restore.

  @returns 0 on success, error code otherwise.
*/
extern "C"
int bcat_reset(st_bstream_image_header *catalogue)
{
  using namespace backup;

  uint n;

  DBUG_ASSERT(catalogue);
  Restore_info *info= static_cast<Restore_info*>(catalogue);
  Logger &log= info->m_log;

  /*
    Iterate over the list of snapshots read from the backup image (and stored
    in snapshot[] array in the catalogue) and for each snapshot create a 
    corresponding Snapshot_info instance. A pointer to this instance is stored
    in m_snap[] array.
   */ 

  for (n=0; n < info->snap_count(); ++n)
  {
    st_bstream_snapshot_info *snap= &info->snapshot[n];

    DBUG_PRINT("restore",("Creating info for snapshot no. %d", n));

    switch (snap->type) {

    case BI_NATIVE:
    {
      backup::LEX_STRING name_lex(snap->engine.name.begin, snap->engine.name.end);
      storage_engine_ref se= get_se_by_name(name_lex);
      handlerton *hton= se_hton(se);

      if (!se || !hton)
      {
        log.report_error(ER_BACKUP_CANT_FIND_SE, name_lex.str);
        return BSTREAM_ERROR;
      }

      if (!hton->get_backup_engine)
      {
        log.report_error(ER_BACKUP_NO_NATIVE_BE, name_lex.str);
        return BSTREAM_ERROR;
      }

      info->m_snap[n]= new Native_snapshot(log, snap->version, se);
                                                              // reports errors
      break;
    }

    case BI_NODATA:
      info->m_snap[n]= new Nodata_snapshot(log, snap->version);
                                                              // reports errors
      break;

    case BI_CS:
      info->m_snap[n]= new CS_snapshot(log, snap->version);
                                                              // reports errors
      break;

    case BI_DEFAULT:
      info->m_snap[n]= new Default_snapshot(log, snap->version);
                                                              // reports errors
      break;

    default:
      // note: we use convention that snapshots are counted starting from 1.
      log.report_error(ER_BACKUP_UNKNOWN_BE, n + 1);
      return BSTREAM_ERROR;
    }

    if (!info->m_snap[n])
    {
      log.report_error(ER_OUT_OF_RESOURCES);
      return BSTREAM_ERROR;
    }

    info->m_snap[n]->m_num= n + 1;
    log.report_driver(info->m_snap[n]->name());
  }

  return BSTREAM_OK;
}

/**
  Called after reading backup image's catalogue and before processing
  metadata and table data.

  Nothing to do here.
*/
extern "C"
int bcat_close(st_bstream_image_header *catalogue)
{
  return BSTREAM_OK;
}

/**
  Add item to restore catalogue.

  @todo Report errors.
*/
extern "C"
int bcat_add_item(st_bstream_image_header *catalogue, 
                  struct st_bstream_item_info *item)
{
  using namespace backup;

  Restore_info *info= static_cast<Restore_info*>(catalogue);
  Logger &log= info->m_log;

  backup::String name_str(item->name.begin, item->name.end);

  DBUG_EXECUTE_IF("restore_catalog_uppercase_names",
                  my_caseup_str(system_charset_info, name_str.c_ptr());
                  );
  DBUG_PRINT("restore",("Adding item %s of type %d (pos=%ld)",
                        item->name.begin,
                        item->type,
                        item->pos));

  switch (item->type) {

  case BSTREAM_IT_TABLESPACE:
  {
    Image_info::Ts *ts= info->add_ts(name_str, item->pos); // reports errors

    return ts ? BSTREAM_OK : BSTREAM_ERROR;
  }

  case BSTREAM_IT_DB:
  {

    if (lower_case_table_names == 1)
      my_casedn_str(system_charset_info, name_str.c_ptr());
    Image_info::Db *db= info->add_db(name_str, item->pos); // reports errors

    return db ? BSTREAM_OK : BSTREAM_ERROR;
  }

  case BSTREAM_IT_TABLE:
  {
    st_bstream_table_info *it= (st_bstream_table_info*)item;

    DBUG_PRINT("restore",(" table's snapshot no. is %d", it->snap_num));

    Snapshot_info *snap= info->m_snap[it->snap_num];

    if (!snap)
    {
      /* 
        This can happen only if the snapshot number is too big - if we failed
        to create one of the snapshots listed in image's header we would stop
        with error earlier.
       */
      DBUG_ASSERT(it->snap_num >= info->snap_count());
      log.report_error(ER_BACKUP_WRONG_TABLE_BE, it->snap_num + 1);
      return BSTREAM_ERROR;
    }

    Image_info::Db *db= info->get_db(it->base.db->base.pos); // reports errors

    if (!db)
      return BSTREAM_ERROR;

    DBUG_PRINT("restore",(" table's database is %s", db->name().ptr()));

    if (lower_case_table_names == 1)
      my_casedn_str(system_charset_info, name_str.c_ptr());
    Image_info::Table *tbl= info->add_table(*db, name_str, *snap, item->pos); 
                                                             // reports errors
    
    return tbl ? BSTREAM_OK : BSTREAM_ERROR;
  }

  case BSTREAM_IT_VIEW:
  case BSTREAM_IT_SPROC:
  case BSTREAM_IT_SFUNC:
  case BSTREAM_IT_EVENT:
  case BSTREAM_IT_TRIGGER:
  case BSTREAM_IT_PRIVILEGE:
  {
    st_bstream_dbitem_info *it= (st_bstream_dbitem_info*)item;
    
    DBUG_ASSERT(it->db);
    
    Image_info::Db *db= (Image_info::Db*) info->get_db(it->db->base.pos);
  
    DBUG_ASSERT(db);
    
    Image_info::Dbobj *it1= info->add_db_object(*db, item->type, name_str,
                                                item->pos);
    if (!it1)
      return BSTREAM_ERROR;
    
    return BSTREAM_OK;
  }   

  default:
    return BSTREAM_OK;

  } // switch (item->type)
}

/*****************************************************************

   Iterators

 *****************************************************************/

static uint cset_iter;  ///< Used to implement trivial charset iterator.
static uint null_iter;  ///< Used to implement trivial empty iterator.

/// Return pointer to an instance of iterator of a given type.
extern "C"
void* bcat_iterator_get(st_bstream_image_header *catalogue, unsigned int type)
{
  typedef backup::Image_info::Iterator Iterator; // to save some typing

  DBUG_ASSERT(catalogue);

  Backup_info *info= static_cast<Backup_info*>(catalogue);
  backup::Logger &log= info->m_log;

  switch (type) {

  case BSTREAM_IT_PERTABLE: // per-table objects
    return &null_iter;

  case BSTREAM_IT_CHARSET:  // character sets
    cset_iter= 0;
    return &cset_iter;

  case BSTREAM_IT_USER:     // users
    return &null_iter;

  case BSTREAM_IT_TABLESPACE:     // table spaces
  {
    Iterator *it= info->get_tablespaces();
    if (!it) 
    {
      log.report_error(ER_OUT_OF_RESOURCES);
      return NULL;
    }
  
    return it;
  }

  case BSTREAM_IT_DB:       // all databases
  {
    Iterator *it= info->get_dbs();
    if (!it) 
    {
      log.report_error(ER_OUT_OF_RESOURCES);
      return NULL;
    }

    return it;  
  }
  
  case BSTREAM_IT_PERDB:    // per-db objects, except tables
  {
    Iterator *it= info->get_perdb();
  
    if (!it)
    {
      log.report_error(ER_BACKUP_CAT_ENUM);
      return NULL;
    }

    return it;
  }

  case BSTREAM_IT_GLOBAL:   // all global objects
  {
    Iterator *it= info->get_global();

    return it;      // if (!it), error has been logged in get_global()
  }

  default:
    return NULL;

  }
}

/// Return next item pointed by a given iterator and advance it to the next positon.
extern "C"
struct st_bstream_item_info*
bcat_iterator_next(st_bstream_image_header *catalogue, void *iter)
{
  using namespace backup;

  /* If this is the null iterator, return NULL immediately */
  if (iter == &null_iter)
    return NULL;

  static bstream_blob name= {NULL, NULL};

  /*
    If it is cset iterator then cset_iter variable contains iterator position.
    We return only 2 charsets: the utf8 charset used to encode all strings and
    the default server charset.
  */
  if (iter == &cset_iter)
  {
    switch (cset_iter) {
      case 0: name.begin= (backup::byte*)my_charset_utf8_bin.csname; break;
      case 1: name.begin= (backup::byte*)system_charset_info->csname; break;
      default: name.begin= NULL; break;
    }

    name.end= name.begin ? name.begin + strlen((char*)name.begin) : NULL;
    cset_iter++;

    return name.begin ? (st_bstream_item_info*)&name : NULL;
  }

  /*
    In all other cases assume that iter points at instance of
    @c Image_info::Iterator and use this instance to get next item.
   */
  const Image_info::Obj *ptr= (*(Image_info::Iterator*)iter)++;

  return ptr ? (st_bstream_item_info*)(ptr->info()) : NULL;
}

extern "C"
void  bcat_iterator_free(st_bstream_image_header *catalogue, void *iter)
{
  /*
    Do nothing for the null and cset iterators, but delete the
    @c Image_info::Iterator object otherwise.
  */
  if (iter == &null_iter)
    return;

  if (iter == &cset_iter)
    return;

  delete (backup::Image_info::Iterator*)iter;
}

/* db-items iterator */

/** 
  Return pointer to an iterator for iterating over objects inside a given 
  database.
 */
extern "C"
void* bcat_db_iterator_get(st_bstream_image_header *catalogue,
                           st_bstream_db_info *dbi)
{
  DBUG_ASSERT(catalogue);
  DBUG_ASSERT(dbi);
  
  Backup_info *info= static_cast<Backup_info*>(catalogue);
  backup::Logger &log= info->m_log;
  Backup_info::Db *db = info->get_db(dbi->base.pos);

  if (!db)
  {
    log.report_error(ER_BACKUP_UNKNOWN_OBJECT);
    return NULL;
  }

  backup::Image_info::Iterator *it= info->get_db_objects(*db);
  if (!it)
  {
    log.report_error(ER_OUT_OF_RESOURCES);
    return NULL;
  }

  return it;
}

extern "C"
struct st_bstream_dbitem_info*
bcat_db_iterator_next(st_bstream_image_header *catalogue,
                      st_bstream_db_info *db,
                      void *iter)
{
  const backup::Image_info::Obj *ptr= (*(backup::Image_info::Iterator*)iter)++;

  return ptr ? (st_bstream_dbitem_info*)ptr->info() : NULL;
}

extern "C"
void  bcat_db_iterator_free(st_bstream_image_header *catalogue,
                            st_bstream_db_info *db,
                            void *iter)
{
  delete (backup::Image_info::Iterator*)iter;
}


/*****************************************************************

   Services for backup stream library related to meta-data
   manipulation.

 *****************************************************************/

/**
  Create given item using serialization data read from backup image.

  @todo Decide what to do if unknown item type is found. Right now we
  bail out.
 */ 
extern "C"
int bcat_create_item(st_bstream_image_header *catalogue,
                     struct st_bstream_item_info *item,
                     bstream_blob create_stmt,
                     bstream_blob other_meta_data)
{
  using namespace backup;
  using namespace obs;

  DBUG_ASSERT(catalogue);
  DBUG_ASSERT(item);

  Restore_info *info= static_cast<Restore_info*>(catalogue);
  Logger &log= info->m_log;
  THD *thd= info->m_thd;
  int create_err= 0;

  /*
    Check for interruption before creating (and thus first destroying) 
    next object.
  */ 
  if (log.report_killed())
    return BSTREAM_ERROR;

  switch (item->type) {
  
  case BSTREAM_IT_DB:     create_err= ER_BACKUP_CANT_RESTORE_DB; break;
  case BSTREAM_IT_TABLE:  create_err= ER_BACKUP_CANT_RESTORE_TABLE; break;
  case BSTREAM_IT_VIEW:   create_err= ER_BACKUP_CANT_RESTORE_VIEW; break;
  case BSTREAM_IT_SPROC:  create_err= ER_BACKUP_CANT_RESTORE_SROUT; break;
  case BSTREAM_IT_SFUNC:  create_err= ER_BACKUP_CANT_RESTORE_SROUT; break;
  case BSTREAM_IT_EVENT:  create_err= ER_BACKUP_CANT_RESTORE_EVENT; break;
  case BSTREAM_IT_TRIGGER: create_err= ER_BACKUP_CANT_RESTORE_TRIGGER; break;
  case BSTREAM_IT_TABLESPACE: create_err= ER_BACKUP_CANT_RESTORE_TS; break;
  case BSTREAM_IT_PRIVILEGE: create_err= ER_BACKUP_CANT_RESTORE_PRIV; break;
  
  /*
    TODO: Decide what to do when we come across unknown item:
    break the restore process as it is done now or continue
    with a warning?
  */

  default:
    log.report_error(ER_BACKUP_UNKNOWN_OBJECT_TYPE);
    return BSTREAM_ERROR;    
  }

  Image_info::Obj *obj= find_obj(*info, *item);

  if (!obj)
  {
    log.report_error(ER_BACKUP_UNKNOWN_OBJECT);
    return BSTREAM_ERROR;
  }

  backup::String sdata(create_stmt.begin, create_stmt.end);

  DBUG_PRINT("restore",("Creating item of type %d pos %ld: %s",
                         item->type, item->pos, sdata.ptr()));
  /*
    Note: The instance created by Image_info::Obj::materialize() is deleted
    when *info is destroyed.
   */ 
  obs::Obj *sobj= obj->materialize(0, sdata);

  Image_info::Obj::describe_buf buf;
  const char *desc= obj->describe(buf);

  if (!sobj)
  {
    log.report_error(create_err, desc);
    return BSTREAM_ERROR;
  }

  /*
    If the item we are creating is an event or trigger, we don't execute it
    yet. It will be done in @c Backup_restore_ctx::do_restore() after table
    data has been restored.
   */ 
  
  switch (item->type) {

  case BSTREAM_IT_EVENT:
  case BSTREAM_IT_TRIGGER:
    return BSTREAM_OK;

  default: break;
  
  }

  // If we are to create a tablespace, first check if it already exists.

  if (item->type == BSTREAM_IT_TABLESPACE)
  {
    if (obs::find_tablespace(thd, sobj->get_name())) 
    {
      // A tablespace with the same name exists. Nothing more to do.
      DBUG_PRINT("restore",(" skipping tablespace which exists"));
      return BSTREAM_OK;
    }
  }

  // Create the object.

  /*
    We need to check to see if the user exists (grantee) and if not, 
    do not execute the grant. 
  */
  if (item->type == BSTREAM_IT_PRIVILEGE)
  {
    /*
      Issue warning to the user that grant was skipped. 

      @todo Replace write_message() call with the result of the revised
            error handling work in WL#4384 with possible implementation
            via a related bug report.
    */
    if (!obs::check_user_existence(thd, sobj))
    {
      log.report_error(log_level::WARNING,
                       ER_BACKUP_GRANT_SKIPPED,
                       obs::grant_get_grant_info(sobj)->ptr(),
                       obs::grant_get_user_name(sobj)->ptr());
      return BSTREAM_OK;
    }
    /*
      We need to check the grant against the database list to ensure the
      grants have not been altered to apply to another database.
      At the time of fixing Bug#41979 (Routine level grants not restored
      when user is dropped, recreated before restore), a GRANT "statement"
      looks like so:
          15 'bup_user2'@'%'
          29 SELECT(b) ON bup_db_grants.s1
          32 SET character_set_client= binary
          54 GRANT SELECT(b) ON bup_db_grants.s1 TO 'bup_user2'@'%'

      For procedures and functions:
          15 'bup_user1'@'%'
          37 Execute ON PROCEDURE bup_db_grants.p1
          32 SET character_set_client= binary
          62 GRANT Execute ON PROCEDURE bup_db_grants.p1 TO 'bup_user1'@'%'
    */
    ::String db_name;  // db name extracted from grant statement
    char *start;
    char *end;
    int size= 0;

    start= strstr((char *)create_stmt.begin, "ON ") + 3;
    if (!strncmp(start, "PROCEDURE ", 10))
      start+= 10;
    if (!strncmp(start, "FUNCTION ", 9))
      start+= 9;
    end= strstr(start, ".");
    size= end - start;
    db_name.alloc(size);
    db_name.length(0);
    db_name.append(start, size);

    if (lower_case_table_names == 1)
      my_casedn_str(system_charset_info, db_name.c_ptr());

    DBUG_EXECUTE_IF("restore_catalog_uppercase_names_grant",
		    my_caseup_str(system_charset_info, db_name.c_ptr());
		    );

    if (!info->has_db(db_name))
    {
      log.report_error(ER_BACKUP_GRANT_WRONG_DB, create_stmt);
      return BSTREAM_ERROR;
    }
  }

  // Mark that data is being changed.
  info->m_data_changed= TRUE;
  if (sobj->create(thd))
  {
    log.report_error(create_err, desc);
    return BSTREAM_ERROR;
  }
  
  return BSTREAM_OK;
}

/**
  Get serialization string for a given object.
  
  The catalogue should contain @c Image_info::Obj instance corresponding to the
  object described by @c item. This instance should contain pointer to 
  @c obs::Obj instance which can be used for getting the serialization string.

  @todo Decide what to do with the serialization string buffer - is it 
  acceptable to re-use a single buffer as it is done now?
 */ 
extern "C"
int bcat_get_item_create_query(st_bstream_image_header *catalogue,
                               struct st_bstream_item_info *item,
                               bstream_blob *stmt)
{
  using namespace backup;
  using namespace obs;

  DBUG_ASSERT(catalogue);
  DBUG_ASSERT(item);
  DBUG_ASSERT(stmt);

  Backup_info *info= static_cast<Backup_info*>(catalogue);
  Logger &log= info->m_log;
  int meta_err= 0;

  /*
    Check for interruption before proceeding.
  */ 
  if (log.report_killed())
    return BSTREAM_ERROR;

  switch (item->type) {
  
  case BSTREAM_IT_DB:     meta_err= ER_BACKUP_GET_META_DB; break;
  case BSTREAM_IT_TABLE:  meta_err= ER_BACKUP_GET_META_TABLE; break;
  case BSTREAM_IT_VIEW:   meta_err= ER_BACKUP_GET_META_VIEW; break;
  case BSTREAM_IT_SPROC:  meta_err= ER_BACKUP_GET_META_SROUT; break;
  case BSTREAM_IT_SFUNC:  meta_err= ER_BACKUP_GET_META_SROUT; break;
  case BSTREAM_IT_EVENT:  meta_err= ER_BACKUP_GET_META_EVENT; break;
  case BSTREAM_IT_TRIGGER: meta_err= ER_BACKUP_GET_META_TRIGGER; break;
  case BSTREAM_IT_TABLESPACE: meta_err= ER_BACKUP_GET_META_TS; break;
  case BSTREAM_IT_PRIVILEGE: meta_err= ER_BACKUP_GET_META_PRIV; break;
  
  /*
    This can't happen - the item was obtained from the backup kernel.
  */
  default: DBUG_ASSERT(FALSE);
  }

  Image_info::Obj *obj= find_obj(*info, *item);

  /*
    The catalogue should contain the specified object and it should have 
    a corresponding server object instance.
   */ 
  DBUG_ASSERT(obj);
  DBUG_ASSERT(obj->m_obj_ptr);
  
  /*
    Note: Using single buffer here means that the string returned by
    this function will live only until the next call. This should be fine
    given the current ussage of the function inside the backup stream library.
    
    TODO: document this or find better solution for string storage.
   */ 
  
  ::String *buf= &(info->serialization_buf);
  buf->length(0);

  if (obj->m_obj_ptr->serialize(info->m_thd, buf))
  {
    Image_info::Obj::describe_buf dbuf;

    log.report_error(meta_err, obj->describe(dbuf));

    return BSTREAM_ERROR;    
  }

  stmt->begin= (backup::byte*)buf->ptr();
  stmt->end= stmt->begin + buf->length();

  return BSTREAM_OK;
}

/**
  Get extra meta-data (if any) for a given object.
 
  @note Extra meta-data is not used currently.
 */ 
extern "C"
int bcat_get_item_create_data(st_bstream_image_header *catalogue,
                            struct st_bstream_item_info *item,
                            bstream_blob *data)
{
  /* We don't use any extra data now */
  return BSTREAM_EOS;
}


/*************************************************

           Implementation of Table_ref class

 *************************************************/

namespace backup {

/** 
  Produce string identifying the table in internal format (as used by 
  storage engines).
*/
const char* Table_ref::internal_name(char *buf, size_t len) const
{
  uint plen= build_table_filename(buf, len, 
                                  db().name().ptr(), name().ptr(), 
                                  "", /* no extension */ 
                                  0 /* not a temporary table - do conversions */);
  buf[plen]='\0';
  return buf;    
}

/** 
    Produce human readable string identifying the table 
    (e.g. for error reporting)
*/
const char* Table_ref::describe(char *buf, size_t len) const
{
  my_snprintf(buf, len, "`%s`.`%s`", db().name().ptr(), name().ptr());
  return buf;
}

/*
  TODO: remove these functions. Currently they are only used by the myisam 
  native backup engine.
*/

/**
  Build the table list as a TABLE_LIST.

  @param[in]  tables  The list of tables to convert.
  @param[in]  lock    The lock type.

  @retval  TABLE_LIST
*/
TABLE_LIST *build_table_list(const Table_list &tables, thr_lock_type lock)
{
  TABLE_LIST *tl= NULL;

  for( uint tno=0; tno < tables.count() ; tno++ )
  {
    TABLE_LIST *ptr = mk_table_list(tables[tno], lock, ::current_thd->mem_root);
    if (!ptr)
    {
      // Failed to allocate (failure has been reported)
      return NULL;
    }
    tl= link_table_list(*ptr,tl);
  }

  return tl;
}

/**
  Free the TABLE_LIST.

  @param[in]  tables  The list of tables to free.
*/
void free_table_list(TABLE_LIST* tables)
{}

} // backup namespace


/*************************************************

                 Helper functions

 *************************************************/

namespace backup {

/** 
  Build linked @c TABLE_LIST list from a list stored in @c Table_list object.
 
  @note The order of tables in the returned list is different than in the 
  input list (reversed).

  @todo Decide what to do if errors are detected. For example, how to react
  if memory for TABLE_LIST structure could not be allocated?
 */



/*
  The constant is declared here (and memory allocated for it) because
  IBM's xlc compiler requires that. However, the intention was to make it
  a pure symbolic constant (no need to allocate memory). If someone knows
  how to achieve that and keep xlc happy, please let me know. /Rafal
*/ 
const size_t Driver::UNKNOWN_SIZE= static_cast<size_t>(-1);

} // backup namespace
