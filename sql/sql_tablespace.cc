/* Copyright (C) 2000-2004 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* drop and alter of tablespaces */

#include "mysql_priv.h"


static const char *str_ts_command_type[]=
{
  "UNKNOWN",
  "CREATE TABLESPACE",
  "ALTER TABLESPACE",
  "CREATE LOGFILE GROUP",
  "ALTER LOGFILE GROUP",
  "DROP TABLESPACE",
  "DROP LOGFILE GROUP",
  "CHANGE FILE TABLESPACE",
  "ALTER ACCESS MODE TABLESPACE"
};


static const char* get_str_ts_command_type(enum ts_command_type ts_cmd_type)
{
  if (ts_cmd_type < -1 || ts_cmd_type > 7)
    ts_cmd_type= TS_CMD_NOT_DEFINED;
  return str_ts_command_type[ts_cmd_type + 1];
}


int mysql_alter_tablespace(THD *thd, st_alter_tablespace *ts_info)
{
  int error= HA_ADMIN_NOT_IMPLEMENTED;
  handlerton *hton= ts_info->storage_engine;

  DBUG_ENTER("mysql_alter_tablespace");
  /*
    If the user haven't defined an engine, this will fallback to using the
    default storage engine.
  */
  if (hton == NULL || hton->state != SHOW_OPTION_YES)
  {
    hton= ha_default_handlerton(thd);
    if (ts_info->storage_engine != 0)
      push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                          ER_WARN_USING_OTHER_HANDLER,
                          ER(ER_WARN_USING_OTHER_HANDLER),
                          hton_name(hton)->str,
                          ts_info->tablespace_name ? ts_info->tablespace_name
                                                : ts_info->logfile_group_name);
  }

  if (hton->alter_tablespace)
  {
    if ((error= hton->alter_tablespace(hton, thd, ts_info)))
    {
      switch (error) {
        case 1:
          DBUG_RETURN(1);
        case HA_ADMIN_NOT_IMPLEMENTED:
          my_error(ER_COM_UNSUPPORTED, MYF(0), hton_name(hton)->str,
                   get_str_ts_command_type(ts_info->ts_cmd_type));
          break;
        case HA_ERR_TABLESPACE_EXIST:
          my_error(ER_TABLESPACE_EXIST, MYF(0), ts_info->tablespace_name);
          break;
        case HA_ERR_NO_SUCH_TABLESPACE:
          my_error(ER_NO_SUCH_TABLESPACE, MYF(0), ts_info->tablespace_name);
          break;
        case HA_ERR_TABLESPACE_NOT_EMPTY:
          my_error(ER_TABLESPACE_NOT_EMPTY, MYF(0), ts_info->tablespace_name);
          break;
        case HA_ERR_TABLESPACE_DATAFILE_EXIST:
          my_error(ER_TABLESPACE_DATAFILE_EXIST, MYF(0),
                   ts_info->data_file_name);
          break;
        default:
          my_error(error, MYF(0));
      }
      DBUG_RETURN(error);
    }
  }
  else
  {
    my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
                        hton_name(hton)->str,
             "TABLESPACE or LOGFILE GROUP");
    DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
  }
  write_bin_log(thd, FALSE, thd->query, thd->query_length);
  DBUG_RETURN(FALSE);
}
