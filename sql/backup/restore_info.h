#ifndef RESTORE_INFO_H_
#define RESTORE_INFO_H_

#include <backup_kernel.h>
#include <backup/image_info.h>
#include <backup/stream_services.h>

class Restore_info;

namespace backup {

class Logger;
class Input_stream;

int restore_table_data(THD*, Restore_info&, 
                       backup::Input_stream&);

} // backup namespace


/**
  Specialization of @c Image_info which is in restore operation.

  An instance of this class is created by 
  @c Backup_restore_ctx::prepare_for_restore() method, which reads the 
  catalogue from a backup image.
  
  Currently it is not possible to select which objects will be restored. This
  class can only be used to examine what is going to be restored.
 */

class Restore_info: public backup::Image_info
{
 public:

  Backup_restore_ctx &m_ctx;

  Restore_info(Backup_restore_ctx&);
  ~Restore_info();

  bool is_valid() const;

  Image_info::Ts* add_ts(const ::String&, uint);
  Image_info::Db* add_db(const ::String&, uint);
  Image_info::Table* add_table(Image_info::Db&, const ::String&, 
                               backup::Snapshot_info&, ulong);

 private:

  // Prevent copying/assignments
  Restore_info(const Restore_info&);
  Restore_info& operator=(const Restore_info&);

  friend int backup::restore_table_data(THD*, Restore_info&, 
                                        backup::Input_stream&);
  friend int ::bcat_add_item(st_bstream_image_header*,
                             struct st_bstream_item_info*);
};

inline
Restore_info::Restore_info(Backup_restore_ctx &ctx)
  :m_ctx(ctx)
{}

inline
Restore_info::~Restore_info()
{
  /*
    Delete Snapshot_info instances - they are created in bcat_reset(). 
   */
  for (ushort n=0; n < snap_count(); ++n)
    delete m_snap[n];
}

inline
bool Restore_info::is_valid() const
{
  return TRUE; 
}

/// Wrapper around Image_info method which reports errors.
inline
backup::Image_info::Ts* 
Restore_info::add_ts(const ::String &name, uint pos)
{
  Ts *ts= Image_info::add_ts(name, pos);

  if (!ts)
    m_ctx.fatal_error(ER_BACKUP_CATALOG_ADD_TS, name.ptr());

  return ts;
}

/// Wrapper around Image_info method which reports errors.
inline
backup::Image_info::Db* 
Restore_info::add_db(const ::String &name, uint pos)
{
  Db *db= Image_info::add_db(name, pos);

  if (!db)
    m_ctx.fatal_error(ER_BACKUP_CATALOG_ADD_DB, name.ptr());

  return db;
}

/// Wrapper around Image_info method which reports errors.
inline
backup::Image_info::Table* 
Restore_info::add_table(Image_info::Db &db, const ::String &name, 
                        backup::Snapshot_info &snap, ulong pos)
{
  Table *t= Image_info::add_table(db, name, snap, pos);

  if (!t)
    m_ctx.fatal_error(ER_BACKUP_CATALOG_ADD_TABLE, db.name().ptr(), name.ptr());

  return t;
}

#endif /*RESTORE_INFO_H_*/
