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

#ifndef LIST_TABLES_HPP
#define LIST_TABLES_HPP

#include <Bitmask.hpp>
#include "SignalData.hpp"

class ListTablesData {
public:
  Uint32 getTableId() {
    return tableId;
  }
  void setTableId(Uint32 val) {
    tableId = val;
  }
  Uint32 getTableType() {
    return tableType;
  }
  void setTableType(Uint32 val) {
    tableType = val;
  }
  Uint32 getTableStore() {
    return BitmaskImpl::getField(1, &requestData, 20, 3);
  }
  void setTableStore(Uint32 val) {
    BitmaskImpl::setField(1, &requestData, 20, 3, val);
  }
  Uint32 getTableTemp() {
    return BitmaskImpl::getField(1, &requestData, 23, 1);
  }
  void setTableTemp(Uint32 val) {
    BitmaskImpl::setField(1, &requestData, 23, 1, val);
  }
  Uint32 getTableState() {
    return BitmaskImpl::getField(1, &requestData, 24, 4);
  }
  void setTableState(Uint32 val) {
    BitmaskImpl::setField(1, &requestData, 24, 4, val);
  }
  Uint32 getListNames() {
    return BitmaskImpl::getField(1, &requestData, 28, 1);
  }
  void setListNames(Uint32 val) {
    BitmaskImpl::setField(1, &requestData, 28, 1, val);
  }
  Uint32 getListIndexes() {
    return BitmaskImpl::getField(1, &requestData, 29, 1);
  }
  void setListIndexes(Uint32 val) {
    BitmaskImpl::setField(1, &requestData, 29, 1, val);
  }
public:
  Uint32 requestData;
  Uint32 tableId;
  Uint32 tableType;
};

/**
 * DEPRICATED
 * It is convenient to pack request/response data per table in one
 * 32-bit word...
 *
 */
class OldListTablesData {
public:
  static Uint32 getTableId(Uint32 data) {
    return BitmaskImpl::getField(1, &data, 0, 12);
  }
  static void setTableId(Uint32& data, Uint32 val) {
    BitmaskImpl::setField(1, &data, 0, 12, val);
  }
  static Uint32 getTableType(Uint32 data) {
    return BitmaskImpl::getField(1, &data, 12, 8);
  }
  static void setTableType(Uint32& data, Uint32 val) {
    BitmaskImpl::setField(1, &data, 12, 8, val);
  }
  static Uint32 getTableStore(Uint32 data) {
    return BitmaskImpl::getField(1, &data, 20, 3);
  }
  static void setTableStore(Uint32& data, Uint32 val) {
    BitmaskImpl::setField(1, &data, 20, 3, val);
  }
  static Uint32 getTableTemp(Uint32 data) {
    return BitmaskImpl::getField(1, &data, 23, 1);
  }
  static void setTableTemp(Uint32& data, Uint32 val) {
    BitmaskImpl::setField(1, &data, 23, 1, val);
  }
  static Uint32 getTableState(Uint32 data) {
    return BitmaskImpl::getField(1, &data, 24, 4);
  }
  static void setTableState(Uint32& data, Uint32 val) {
    BitmaskImpl::setField(1, &data, 24, 4, val);
  }
  static Uint32 getListNames(Uint32 data) {
    return BitmaskImpl::getField(1, &data, 28, 1);
  }
  static void setListNames(Uint32& data, Uint32 val) {
    BitmaskImpl::setField(1, &data, 28, 1, val);
  }
  static Uint32 getListIndexes(Uint32 data) {
    return BitmaskImpl::getField(1, &data, 29, 1);
  }
  static void setListIndexes(Uint32& data, Uint32 val) {
    BitmaskImpl::setField(1, &data, 29, 1, val);
  }
};

class ListTablesReq {
  /**
   * Sender(s)
   */
  friend class Backup;
  friend class Table;
  friend class Suma;
  
  /**
   * Reciver(s)
   */
  friend class Dbdict;

public:
  STATIC_CONST( oldSignalLength = 3 );
  STATIC_CONST( SignalLength = 5 );

public:  
  Uint32 senderData;
  Uint32 senderRef;
  Uint32 requestData;
  Uint32 tableId;
  Uint32 tableType;

  Uint32 getTableId() {
    return tableId;
  }
  void setTableId(Uint32 val) {
    tableId = val;
  }
  Uint32 getTableType() const {
    return tableType;
  }
  void setTableType(Uint32 val) {
    tableType = val;
  }
  Uint32 getListNames() const {
    ListTablesData* ltd = (ListTablesData *) &requestData;
    return ltd->getListNames();
  }
  void setListNames(Uint32 val) {
    ListTablesData* ltd = (ListTablesData *) &requestData;
    ltd->setListNames(val);
  }
  Uint32 getListIndexes() const {
    ListTablesData* ltd = (ListTablesData *) &requestData;
    return ltd->getListIndexes();
  }
  void setListIndexes(Uint32 val) {
    ListTablesData* ltd = (ListTablesData *) &requestData;
    ltd->setListIndexes(val);
  }


  /* For backwards compatility */
  Uint32 oldGetTableId() {
    return OldListTablesData::getTableId(requestData);
  }
  void oldSetTableId(Uint32 val) {
    OldListTablesData::setTableId(requestData, val);
  }
  Uint32 oldGetTableType() const {
    return OldListTablesData::getTableType(requestData);
  }
  void oldSetTableType(Uint32 val) {
    OldListTablesData::setTableType(requestData, val);
  }
};

class ListTablesConf {
  /**
   * Sender(s)
   */
  friend class Dbdict;
  
  /**
   * Reciver(s)
   */
  friend class Backup;
  friend class Table;

public:
  STATIC_CONST( SignalLength = 2 );

public:
  Uint32 senderData;
  Uint32 noOfTables;

  SECTION( TABLE_DATA = 0 );
  SECTION( TABLE_NAMES = 1 );
};

class OldListTablesConf {
  /**
   * Sender(s)
   */
  friend class Dbdict;

  /**
   * Reciver(s)
   */
  friend class Backup;
  friend class Table;

public:
  /**
   * Note: last signal is indicated by having length < 25
   */
  STATIC_CONST( SignalLength = 25 );
  STATIC_CONST( HeaderLength = 2  );
  STATIC_CONST( DataLength   = 23 );

public:  
  Uint32 senderData;
  Uint32 counter;
  Uint32 tableData[DataLength];

  static Uint32 getTableId(Uint32 data) {
    return OldListTablesData::getTableId(data);
  }
  void setTableId(unsigned pos, Uint32 val) {
    OldListTablesData::setTableId(tableData[pos], val);
  }
  static Uint32 getTableType(Uint32 data) {
    return OldListTablesData::getTableType(data);
  }
  void setTableType(unsigned pos, Uint32 val) {
    OldListTablesData::setTableType(tableData[pos], val);
  }
  static Uint32 getTableStore(Uint32 data) {
    return OldListTablesData::getTableStore(data);
  }
  void setTableStore(unsigned pos, Uint32 val) {
    OldListTablesData::setTableStore(tableData[pos], val);
  }
  static Uint32 getTableState(Uint32 data) {
    return OldListTablesData::getTableState(data);
  }
  void setTableState(unsigned pos, Uint32 val) {
    OldListTablesData::setTableState(tableData[pos], val);
  }
  static Uint32 getTableTemp(Uint32 data) {
    return OldListTablesData::getTableTemp(data);
  }
  void setTableTemp(unsigned pos, Uint32 val) {
    OldListTablesData::setTableTemp(tableData[pos], val);
  }
};

#endif
