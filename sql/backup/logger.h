#ifndef _BACKUP_LOGGER_H
#define _BACKUP_LOGGER_H

#include <backup/error.h>


namespace backup {

/// Logging levels for messages generated by backup system
struct log_level {
 enum value {
    INFO=    MYSQL_ERROR::WARN_LEVEL_NOTE,
    WARNING= MYSQL_ERROR::WARN_LEVEL_WARN,
    ERROR=   MYSQL_ERROR::WARN_LEVEL_ERROR
 };
};


/**
  This class exposes API used to output various messages during backup/restore
  process.

  Destination of the messages is determined by the implementation. Currently
  messages are output the the servers error log.

  Messages are intended for a DBA and thus should be localized. A message should
  be registered in errmsg.txt database and have assigned an error code. Printing
  message corresponding to an error code is done using @c report_error() methods.
 */
class Logger
{
 public:

   Logger(): m_save_errors(FALSE)
   {}

   int write_message(log_level::value level , int error_code, const char *msg);

   int write_message(log_level::value level, const char *msg, ...)
   {
     va_list args;

     va_start(args,msg);
     int res= v_write_message(level, 0, msg, args);
     va_end(args);

     return res;
   }

   /// Reports error with log_level::ERROR.
   int report_error(int error_code, ...)
   {
     va_list args;

     va_start(args,error_code);
     int res= v_report_error(log_level::ERROR, error_code, args);
     va_end(args);

     return res;
   }

   /// Reports error with registered error description string.
   int report_error(log_level::value level, int error_code, ...)
   {
     va_list args;

     va_start(args,error_code);
     int res= v_report_error(level, error_code, args);
     va_end(args);

     return res;
   }

   /// Reports error with given description.
   int report_error(const char *format, ...)
   {
     va_list args;

     va_start(args,format);
     int res= v_write_message(log_level::ERROR, 0, format, args);
     va_end(args);

     return res;
   }

   ///  Request that all reported errors are saved in the logger.
   void save_errors()
   {
     if (m_save_errors)
       return;
     clear_saved_errors();
     m_save_errors= TRUE;
   }

   /// Stop saving errors.
   void stop_save_errors()
   {
     if (!m_save_errors)
       return;
     m_save_errors= FALSE;
   }

   /// Delete all saved errors to free resources.
   void clear_saved_errors()
   { errors.delete_elements(); }

   /// Return a pointer to most recent saved error.
   MYSQL_ERROR *last_saved_error()
   { return errors.head(); }

 private:

  List<MYSQL_ERROR> errors;  ///< Used to store saved errors
  bool m_save_errors;        ///< Flag telling if errors should be saved

  int v_report_error(log_level::value,int,va_list);
  int v_write_message(log_level::value,int, const char*,va_list);
};


} // backup namespace

#endif
