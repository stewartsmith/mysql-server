#ifndef _BACKUP_KERNEL_API_H
#define _BACKUP_KERNEL_API_H

#include <backup/logger.h>
#include <backup/stream_services.h>

/**
  @file

  Functions and types forming the backup kernel API
*/


/**
  @brief Size of the buffer used for transfers between backup kernel and
  backup/restore drivers.
*/
#define DATA_BUFFER_SIZE  (1024*1024)

/*
  Functions used to initialize and shut down the online backup system.
  
  Note: these functions are called at plugin load and plugin shutdown time,
  respectively.
 */ 
int backup_init();
void backup_shutdown();

/*
  Called from the big switch in mysql_execute_command() to execute
  backup related statement
*/
int execute_backup_command(THD*, LEX*);

// forward declarations

class Backup_info;
class Restore_info;

namespace backup {

class Mem_allocator;
class Stream;
class Output_stream;
class Input_stream;
class Native_snapshot;

int write_table_data(THD*, Backup_info&, Output_stream&);
int restore_table_data(THD*, Restore_info&, Input_stream&);

}

/**
  Instances of this class are used for creating required context and performing
  backup/restore operations.
  
  @see kernel.cc
 */ 
class Backup_restore_ctx: public backup::Logger 
{
 public:

  Backup_restore_ctx(THD*);
  ~Backup_restore_ctx();

  bool is_valid() const;
  ulonglong op_id() const;

  Backup_info*  prepare_for_backup(LEX_STRING location, const char*, bool);
  Restore_info* prepare_for_restore(LEX_STRING location, const char*);  

  int do_backup();
  int do_restore();
  int fatal_error(int, ...);

  int close();

  THD* thd() const { return m_thd; }

 private:

  /** Indicates if a backup/restore operation is in progress. */
  static bool is_running;
  static pthread_mutex_t  run_lock; ///< To guard @c is_running flag.

  /** 
    @brief State of a context object. 
    
    Backup/restore can be performed only if object is prepared for that operation.
   */
  enum { CREATED,
         PREPARED_FOR_BACKUP,
         PREPARED_FOR_RESTORE,
         CLOSED } m_state;

  ulonglong m_thd_options;  ///< For saving thd->options.
  /**
    If backup/restore was interrupted by an error, this member stores the error 
    number.
   */ 
  int m_error;
  
  const char *m_path;   ///< Path to where the backup image file is located.

  /** If true, the backup image file is deleted at clean-up time. */
  bool m_remove_loc;

  backup::Stream *m_stream; ///< Pointer to the backup stream object, if opened.
  backup::Image_info *m_catalog;  ///< Pointer to the image catalogue object.

  /** Memory allocator for backup stream library. */
  static backup::Mem_allocator *mem_alloc;

  int prepare(LEX_STRING location);
  void disable_fkey_constraints();
  
  friend class Backup_info;
  friend class Restore_info;
  friend int backup_init();
  friend void backup_shutdown();
  friend bstream_byte* bstream_alloc(unsigned long int);
  friend void bstream_free(bstream_byte *ptr);
};

/// Check if instance is correctly created.
inline
bool Backup_restore_ctx::is_valid() const
{
  return m_error == 0;
}

/// Return global id of the backup/restore operation.
inline
ulonglong Backup_restore_ctx::op_id() const
{
  return m_op_id; // inherited from Logger class
}

/// Disable foreign key constraint checks (needed during restore).
inline
void Backup_restore_ctx::disable_fkey_constraints()
{
  m_thd->options|= OPTION_NO_FOREIGN_KEY_CHECKS;
}

/**
  Report error and move context object into error state.
  
  After this method is called the context object is in error state and
  cannot be normally used. It still can be examined for saved error messages.
  The code of the error reported here is saved in m_error member.
  
  Only one fatal error can be reported. If context is already in error
  state when this method is called, it does nothing.
  
  @return error code given as input or stored in the context object if
  a fatal error was reported before.
 */ 
inline
int Backup_restore_ctx::fatal_error(int error_code, ...)
{
  if (m_error)
    return m_error;

  va_list args;

  m_error= error_code;
  m_remove_loc= TRUE;

  va_start(args,error_code);
  v_report_error(backup::log_level::ERROR, error_code, args);
  va_end(args);

  return error_code;
}

/*
  Now, when Backup_restore_ctx is defined, include definitions
  of Backup_info and Restore_info classes.
*/

#include <backup/backup_info.h>
#include <backup/restore_info.h>

#endif
