/* Copyright (C) 2003 MySQL AB

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

#ifndef atrt_config_hpp
#define atrt_config_hpp

#include <ndb_global.h>
#include <Vector.hpp>
#include <BaseString.hpp>
#include <Logger.hpp>
#include <mgmapi.h>
#include <CpcClient.hpp>
#include <Properties.hpp>
#include <mysql.h>

enum ErrorCodes 
{
  ERR_OK = 0,
  ERR_NDB_FAILED = 101,
  ERR_SERVERS_FAILED = 102,
  ERR_MAX_TIME_ELAPSED = 103,
  ERR_COMMAND_FAILED = 104,
  ERR_FAILED_TO_START = 105
};

struct atrt_host 
{
  size_t m_index;
  BaseString m_user;
  BaseString m_basedir;
  BaseString m_hostname;
  SimpleCpcClient * m_cpcd;
  Vector<struct atrt_process*> m_processes;
};

struct atrt_options
{
  enum Feature {
    AO_REPLICATION = 1,
    AO_NDBCLUSTER = 2
  };
  
  int m_features;
  Properties m_loaded;
  Properties m_generated;
};

struct atrt_process 
{
  size_t m_index;
  struct atrt_host * m_host;
  struct atrt_cluster * m_cluster;

  enum Type {
    AP_ALL          = 255
    ,AP_NDBD         = 1
    ,AP_NDB_API      = 2
    ,AP_NDB_MGMD     = 4
    ,AP_MYSQLD       = 16
    ,AP_CLIENT       = 32
    ,AP_CLUSTER      = 256 // Used for options parsing for "cluster" options
  } m_type;

  SimpleCpcClient::Process m_proc;

  NdbMgmHandle m_ndb_mgm_handle;   // if type == ndb_mgm
  atrt_process * m_mysqld;         // if type == client
  atrt_process * m_rep_src;        // if type == mysqld
  Vector<atrt_process*> m_rep_dst; // if type == mysqld
  MYSQL m_mysql;                   // if type == mysqld
  atrt_options m_options;
  uint m_nodeid;                   // if m_fix_nodeid

  struct {
    bool m_saved;
    SimpleCpcClient::Process m_proc;
  } m_save;

};

struct atrt_cluster
{
  BaseString m_name;
  BaseString m_dir;
  Vector<atrt_process*> m_processes;
  atrt_options m_options;
  uint m_next_nodeid;                   // if m_fix_nodeid
};

struct atrt_config 
{
  bool m_generated;
  BaseString m_key;
  BaseString m_replication;
  Vector<atrt_host*> m_hosts;
  Vector<atrt_cluster*> m_clusters;
  Vector<atrt_process*> m_processes;
};

struct atrt_testcase 
{
  bool m_report;
  bool m_run_all;
  time_t m_max_time;
  BaseString m_command;
  BaseString m_args;
  BaseString m_name;
};

extern Logger g_logger;

void require(bool x);
bool parse_args(int argc, char** argv);
bool setup_config(atrt_config&, const char * mysqld);
bool configure(atrt_config&, int setup);
bool setup_directories(atrt_config&, int setup);
bool setup_files(atrt_config&, int setup, int sshx);

bool deploy(atrt_config&);
bool sshx(atrt_config&, unsigned procmask);
bool start(atrt_config&, unsigned procmask);

bool remove_dir(const char *, bool incl = true);
bool connect_hosts(atrt_config&);
bool connect_ndb_mgm(atrt_config&);
bool wait_ndb(atrt_config&, int ndb_mgm_node_status);
bool start_processes(atrt_config&, int);
bool stop_processes(atrt_config&, int);
bool update_status(atrt_config&, int);
int is_running(atrt_config&, int);
bool gather_result(atrt_config&, int * result);

bool read_test_case(FILE *, atrt_testcase&, int& line);
bool setup_test_case(atrt_config&, const atrt_testcase&);

bool setup_hosts(atrt_config&);

bool do_command(atrt_config& config);

bool start_process(atrt_process & proc);
bool stop_process(atrt_process & proc);

/**
 * check configuration if any changes has been 
 *   done for the duration of the latest running test
 *   if so, return true, and reset those changes
 *   (true, indicates that a restart is needed to actually
 *    reset the running processes)
 */
bool reset_config(atrt_config&);

NdbOut&
operator<<(NdbOut& out, const atrt_process& proc);

/**
 * SQL
 */
bool setup_db(atrt_config&);

/**
 * Global variables...
 */
extern Logger g_logger;

extern const char * g_cwd;
extern const char * g_my_cnf;
extern const char * g_user;
extern const char * g_basedir;
extern const char * g_prefix;
extern const char * g_prefix1;
extern int          g_baseport;
extern int          g_fqpn;
extern int          g_fix_nodeid;
extern int          g_default_ports;

extern const char * g_clusters;

#endif
