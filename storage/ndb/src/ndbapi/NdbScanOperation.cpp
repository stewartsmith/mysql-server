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

#include <ndb_global.h>
#include <Ndb.hpp>
#include <NdbScanOperation.hpp>
#include <NdbIndexScanOperation.hpp>
#include <NdbTransaction.hpp>
#include "NdbApiSignal.hpp"
#include <NdbOut.hpp>
#include "NdbDictionaryImpl.hpp"
#include <NdbBlob.hpp>
#include <NdbInterpretedCode.hpp>

#include <NdbRecAttr.hpp>
#include <NdbReceiver.hpp>

#include <stdlib.h>
#include <NdbSqlUtil.hpp>
#include <AttributeHeader.hpp>

#include <signaldata/ScanTab.hpp>
#include <signaldata/KeyInfo.hpp>
#include <signaldata/AttrInfo.hpp>
#include <signaldata/TcKeyReq.hpp>

#define DEBUG_NEXT_RESULT 0

NdbScanOperation::NdbScanOperation(Ndb* aNdb, NdbOperation::Type aType) :
  NdbOperation(aNdb, aType),
  m_transConnection(NULL)
{
  theParallelism = 0;
  m_allocated_receivers = 0;
  m_prepared_receivers = 0;
  m_api_receivers = 0;
  m_conf_receivers = 0;
  m_sent_receivers = 0;
  m_receivers = 0;
  m_array = new Uint32[1]; // skip if on delete in fix_receivers
  theSCAN_TABREQ = 0;
  m_executed = false;
  m_scan_buffer= NULL;
  m_scanUsingOldApi= false;
  m_interpretedCodeOldApi= NULL;
}

NdbScanOperation::~NdbScanOperation()
{
  for(Uint32 i = 0; i<m_allocated_receivers; i++){
    m_receivers[i]->release();
    theNdb->releaseNdbScanRec(m_receivers[i]);
  }
  delete[] m_array;
}

void
NdbScanOperation::setErrorCode(int aErrorCode){
  NdbTransaction* tmp = theNdbCon;
  theNdbCon = m_transConnection;
  NdbOperation::setErrorCode(aErrorCode);
  theNdbCon = tmp;
}

void
NdbScanOperation::setErrorCodeAbort(int aErrorCode){
  NdbTransaction* tmp = theNdbCon;
  theNdbCon = m_transConnection;
  NdbOperation::setErrorCodeAbort(aErrorCode);
  theNdbCon = tmp;
}

  
/*****************************************************************************
 * int init();
 *
 * Return Value:  Return 0 : init was successful.
 *                Return -1: In all other case.  
 * Remark:        Initiates operation record after allocation.
 *****************************************************************************/
int
NdbScanOperation::init(const NdbTableImpl* tab, NdbTransaction* myConnection)
{
  m_transConnection = myConnection;
  //NdbConnection* aScanConnection = theNdb->startTransaction(myConnection);
  theNdb->theRemainingStartTransactions++; // will be checked in hupp...
  NdbTransaction* aScanConnection = theNdb->hupp(myConnection);
  if (!aScanConnection){
    theNdb->theRemainingStartTransactions--;
    setErrorCodeAbort(theNdb->getNdbError().code);
    return -1;
  }

  // NOTE! The hupped trans becomes the owner of the operation
  if(NdbOperation::init(tab, aScanConnection, false) != 0){
    theNdb->theRemainingStartTransactions--;
    return -1;
  }
  
  initInterpreter();
  
  theStatus = GetValue;
  theOperationType = OpenScanRequest;
  theNdbCon->theMagicNumber = 0xFE11DF;
  theNoOfTupKeyLeft = tab->m_noOfDistributionKeys;
  m_ordered= false;
  m_descending= false;
  m_read_range_no = 0;
  m_executed = false;
  m_scanUsingOldApi= false;
  m_interpretedCodeOldApi= NULL;
  return 0;
}

int
NdbScanOperation::handleScanGetValuesOldApi()
{
  /* Handle old API-defined scan getValue(s) */
  assert(m_scanUsingOldApi);

  // TODO : Is it valid for an old-Api scan to have no extra
  // getValues()?
  if (theReceiver.theFirstRecAttr != NULL) 
  {
    /* theReceiver has a list of RecAttrs which the user
     * wants to read.  Traverse it, adding signals to the
     * request to read them, *similar* to extra GetValue
     * handling, except that we want to use the RecAttrs we've
     * already got.
     * Once these are added to the signal train, all other handling
     * is exactly the same as for normal NdbRecord 'extra GetValues'
     */
    NdbRecAttr* recAttrToRead = theReceiver.theFirstRecAttr;

    while(recAttrToRead != NULL)
    {
      int res;
      Uint32 ah;
      AttributeHeader::init(&ah, recAttrToRead->theAttrId, 0);
      res= insertATTRINFO(ah);
      if (res==-1)
        return -1;
      theInitialReadSize= theTotalCurrAI_Len - AttrInfo::SectionSizeInfoLength;
      recAttrToRead= recAttrToRead->next();
    } 
  }

  return 0;
}

/* Method for adding interpreted code signals to a 
 * scan operation request.
 * Both main program words and subroutine words can 
 * be added in one method as scans do not use 
 * the final update or final read sections.
 */
int
NdbScanOperation::addInterpretedCode(Uint32 aTC_ConnectPtr,
                                     Uint64 aTransId)
{
  Uint32 mainProgramWords= 0;
  Uint32 subroutineWords= 0;

  /* Where to start writing the new AIs */
  Uint32 *attrInfoPtr= theATTRINFOptr;
  Uint32 remain= AttrInfo::MaxSignalLength - theAI_LenInCurrAI;
  const NdbInterpretedCode *code= m_interpreted_code;

  /* Main program size depends on whether there's subroutines */
  mainProgramWords= code->m_first_sub_instruction_pos ?
    code->m_first_sub_instruction_pos :
    code->m_instructions_length;
  
  int res = insertATTRINFOData_NdbRecord(aTC_ConnectPtr, aTransId,
                                       (const char *)code->m_buffer,
                                       mainProgramWords << 2,
                                       &attrInfoPtr, &remain);

  if (res == 0)
  {
    /* Add subroutines, if we have any */
    if (code->m_number_of_subs > 0)
    {
      assert(mainProgramWords > 0);
      assert(code->m_first_sub_instruction_pos > 0);
      
      Uint32 *subroutineStart= 
        &code->m_buffer[ code->m_first_sub_instruction_pos ];
      subroutineWords= 
        code->m_instructions_length -
        code->m_first_sub_instruction_pos;
      
      res = insertATTRINFOData_NdbRecord(aTC_ConnectPtr, aTransId,
                                         (const char *)subroutineStart,
                                         subroutineWords << 2,
                                         &attrInfoPtr, &remain);
    }

    /* Update signal section lengths */
    theInterpretedSize= mainProgramWords;
    theSubroutineSize= subroutineWords;
  }

  /* Need to keep theAI_LenInCurrAI up to date although 
   * insertATTRINFOData_NdbRecord does not.  Needed for setting
   * the last AI length in prepareSendScan
   */
  theAI_LenInCurrAI= theCurrentATTRINFO->getLength();

  return res;
};

/* Method for handling scanoptions passed into 
 * NdbTransaction::scanTable or scanIndex
 */
int
NdbScanOperation::handleScanOptions(const ScanOptions *options)
{
  /* Options size has already been checked.
   * scan_flags, parallel and batch have been handled
   * already (see NdbTransaction::scanTable and scanIndex)
   */
  if ((options->optionsPresent & ScanOptions::SO_GETVALUE) &&
      (options->numExtraGetValues > 0))
  {
    if (options->extraGetValues == NULL)
    {
      setErrorCodeAbort(4299);
      /* Incorrect combination of ScanOption flags, 
       * extraGetValues ptr and numExtraGetValues */
      return -1;
    }

    /* Add extra getValue()s */
    for (unsigned int i=0; i < options->numExtraGetValues; i++)
    {
      NdbOperation::GetValueSpec *pvalSpec = &(options->extraGetValues[i]);

      pvalSpec->recAttr=NULL;

      if (pvalSpec->column == NULL)
      {
        setErrorCodeAbort(4295);
        // Column is NULL in Get/SetValueSpec structure
        return -1;
      }

      /* Call internal NdbRecord specific getValue() method
       * Same method handles table scans and index scans
       */
      NdbRecAttr *pra=
        getValue_NdbRecord_scan(&NdbColumnImpl::getImpl(*pvalSpec->column),
                                (char *) pvalSpec->appStorage);
        
      if (pra == NULL)
      {
        return -1;
      }
      
      pvalSpec->recAttr = pra;
    }
  }

  if (options->optionsPresent & ScanOptions::SO_PARTITION_ID)
  {
    /* Should not have any blobs defined at this stage */
    assert(theBlobList == NULL);
    theDistributionKey = options->partitionId;
    theDistrKeyIndicator_ = 1;
    DBUG_PRINT("info", ("NdbScanOperation::handleScanOptions(dist key): %u",
                        theDistributionKey));
  }

  if (options->optionsPresent & ScanOptions::SO_INTERPRETED)
  {
    /* Check the program's for the same table as the
     * operation
     * TODO : Online alter tables that don't affect InterpretedCode objects
     * (i.e. extra columns at end of table, new indices), should not affect
     * pre-finalised NdbInterpretedCode objects.  How do we tell if two
     * tables are the same except for insignificant schema changes?
     */
    const NdbDictionary::Table* codeTable= options->interpretedCode->getTable();
    if ((codeTable != NULL) && 
        (codeTable != m_currentTable))
    {
      setErrorCodeAbort(4524); // NdbInterpretedCode is for different table
      return -1;
    } 

    if ((options->interpretedCode->m_flags & 
         NdbInterpretedCode::Finalised) == 0)
    {
      setErrorCodeAbort(4519);
      return -1; // NdbInterpretedCode::finalise() not called.
    }
    m_interpreted_code= options->interpretedCode;
  }

  return 0;
}

/* scanTableImpl is called from NdbTransaction::scanTable() to 
 * define a scan operation
 */
int
NdbScanOperation::scanTableImpl(const NdbRecord *result_record,
                                NdbOperation::LockMode lock_mode,
                                const unsigned char *result_mask,
                                const NdbScanOperation::ScanOptions *options,
                                Uint32 sizeOfOptions)
{
  NdbBlob *lastBlob;
  Uint32 column_count;
  int res;
  bool haveBlob= false;
  Uint32 scan_flags = 0;
  Uint32 parallel = 0;
  Uint32 batch = 0;

  if (options != NULL)
  {
    /* Check options size for versioning... */
    if (unlikely((sizeOfOptions !=0) &&
                 (sizeOfOptions != sizeof(ScanOptions))))
    {
      /* Handle different sized ScanOptions
       * Probably smaller is old version, larger is new version
       */
      
      /* No other versions supported currently */
      setErrorCodeAbort(4298);
      /* Invalid or unsupported ScanOptions structure */
      return -1;
    }
    
    /* Process some initial ScanOptions - most are 
     * handled later
     */
    if (options->optionsPresent & ScanOptions::SO_SCANFLAGS)
      scan_flags = options->scan_flags;
    if (options->optionsPresent & ScanOptions::SO_PARALLEL)
      parallel = options->parallel;
    if (options->optionsPresent & ScanOptions::SO_BATCH)
      batch = options->batch;
  }
  if (result_record->flags & NdbRecord::RecIsIndex)
  {
    setErrorCodeAbort(4340);
    return -1;
  }

  /* Process scan definition info */
  res= processTableScanDefs(lock_mode, scan_flags, parallel, batch);
  if (res == -1)
    return -1;

  result_record->copyMask(m_read_mask, result_mask);

  lastBlob= NULL;
  column_count= 0;
  for (Uint32 i= 0; i<result_record->noOfColumns; i++)
  {
    const NdbRecord::Attr *col;
    Uint32 ah;
    Uint32 attrId;

    col= &result_record->columns[i];

    /* Skip column if result_mask says so. But cannot mask pseudo columns. */
    attrId= col->attrId;
    if (!(attrId & AttributeHeader::PSEUDO) &&
        !BitmaskImpl::get((NDB_MAX_ATTRIBUTES_IN_TABLE+31)>>5,
                          m_read_mask, attrId))
      continue;

    /* Blob reads are handled with a getValue() in NdbBlob.cpp. */
    if (unlikely(col->flags & NdbRecord::IsBlob))
    {
      m_keyInfo= 1;                         // Need keyinfo for blob scan
      haveBlob= true;
      continue;
    }

    AttributeHeader::init(&ah, attrId, 0);
    res= insertATTRINFO(ah);
    if (res==-1)
      return -1;

    if (col->flags & NdbRecord::IsDisk)
      m_no_disk_flag= false;
    column_count++;
  }
  theReceiver.m_record.m_column_count= column_count;

  theInitialReadSize= theTotalCurrAI_Len - AttrInfo::SectionSizeInfoLength;
  theStatus= NdbOperation::UseNdbRecord;
  m_attribute_record= result_record;

  /* Handle any getValue() calls made against the old API. */
  if (m_scanUsingOldApi)
  {
    if (handleScanGetValuesOldApi() !=0)
      return -1;
  }

  /* Handle scan options - always for old style scan API */
  if (options != NULL)
  {
    if (handleScanOptions(options) != 0)
      return -1;
  }

  /* Get Blob handles unless this is an old Api scan op 
   * For old Api Scan ops, the Blob handles are already
   * set up by the call to getBlobHandle()
   */
  if (unlikely(haveBlob) && !m_scanUsingOldApi)
  {
    if (getBlobHandlesNdbRecord(m_transConnection) == -1)
      return -1;
  }

  /* Add interpreted code words to ATTRINFO signal
   * chain as necessary
   */
  if (m_interpreted_code != NULL)
  {
    if (addInterpretedCode(theNdbCon->theTCConPtr,
                           theNdbCon->theTransactionId) == -1)
      return -1;
  }
  
  /* Scan is now fully defined, so let's start preparing
   * signals.
   */
  if (prepareSendScan(theNdbCon->theTCConPtr, 
                      theNdbCon->theTransactionId) == -1)
    /* Error code should be set */
    return -1;
  
  return 0;
}

/*
  Compare two rows on some prefix of the index.
  This is used to see if we can determine that all rows in an index range scan
  will come from a single fragment (if the two rows bound a single distribution
  key).
 */
static int
compare_index_row_prefix(const NdbRecord *rec,
                         const char *row1,
                         const char *row2,
                         Uint32 prefix_length)
{
  Uint32 i;

  for (i= 0; i<prefix_length; i++)
  {
    const NdbRecord::Attr *col= &rec->columns[rec->key_indexes[i]];

    bool is_null1= col->is_null(row1);
    bool is_null2= col->is_null(row2);
    if (is_null1)
    {
      if (!is_null2)
        return -1;
      /* Fall-through to compare next one. */
    }
    else
    {
      if (is_null2)
        return 1;

      Uint32 offset= col->offset;
      Uint32 maxSize= col->maxSize;
      const char *ptr1= row1 + offset;
      const char *ptr2= row2 + offset;
      void *info= col->charset_info;
      int res=
        (*col->compare_function)(info, ptr1, maxSize, ptr2, maxSize, true);
      if (res)
      {
        assert(res != NdbSqlUtil::CmpUnknown);
        return res;
      }
    }
  }

  return 0;
}

void
NdbIndexScanOperation::setDistKeyFromRange(const NdbRecord *key_record,
                                           const NdbRecord *result_record,
                                           const char *row,
                                           Uint32 distkeyMax)
{
  Uint64 tmp[1000];
  Ndb::Key_part_ptr ptrs[NDB_MAX_NO_OF_ATTRIBUTES_IN_KEY+1];
  Uint32 i;
  for (i = 0; i<key_record->distkey_index_length; i++)
  {
    const NdbRecord::Attr *col =
      &key_record->columns[key_record->distkey_indexes[i]];
    ptrs[i].ptr = row + col->offset;
    ptrs[i].len = col->maxSize;
  }
  ptrs[i].ptr = 0;
  
  Uint32 hashValue;
  int ret = Ndb::computeHash(&hashValue, result_record->table,
                             ptrs, tmp, sizeof(tmp));
  if (ret == 0)
  {
    theDistributionKey= result_record->table->getPartitionId(hashValue);
    theDistrKeyIndicator_= 1;
  }
#ifdef VM_TRACE
  else
  {
    ndbout << "err: " << ret << endl;
    assert(false);
  }
#endif
}


/** 
 * setBound()
 *
 * This method is called from scanIndex() and setBound().  
 * It adds a bound to an Index Scan.
 */
int 
NdbIndexScanOperation::setBound(const NdbRecord *key_record,
                                const IndexBound& bound)
{
  /*
    Set up index range bounds, write into keyinfo.
    
    ToDo: We only set scan distribution key if there's only one
    scan bound. (see BUG#25821).  MRR/BKA does not use it.
  */

  if (unlikely((theStatus != NdbOperation::UseNdbRecord)))
  {
    setErrorCodeAbort(4284);
    /* Cannot mix NdbRecAttr and NdbRecord methods in one operation */
    return -1;
  }

  if (unlikely(key_record == NULL))
  {
    setErrorCodeAbort(4285);
    /* NULL NdbRecord pointer */
    return -1;
  }

  m_num_bounds++;

  if (unlikely((m_num_bounds > 1) &&
               (m_multi_range == 0)))
  {
    /* > 1 IndexBound, but not MRR */
    setErrorCodeAbort(4509);
    /* Non SF_MultiRange scan cannot have more than one bound */
    return -1;
  }

  Uint32 j;
  Uint32 key_count, common_key_count;
  Uint32 range_no;
  Uint32 bound_head;

  range_no= bound.range_no;
  if (unlikely(range_no > MaxRangeNo))
  {
    setErrorCodeAbort(4286);
    return -1;
  }

  /* Check valid ordering of supplied range numbers */
  if ( m_read_range_no && m_ordered )
  {
    if (unlikely((m_num_bounds > 1) &&
                 (range_no <= m_previous_range_num))) 
    {
      setErrorCodeAbort(4282);
      /* range_no not strictly increasing in ordered multi-range index scan */
      return -1;
    }
    
    m_previous_range_num= range_no;
  }

  key_count= bound.low_key_count;
  common_key_count= key_count;
  if (key_count < bound.high_key_count)
    key_count= bound.high_key_count;
  else
    common_key_count= bound.high_key_count;

  if (unlikely(key_count > key_record->key_index_length))
  {
    /* Too many keys specified for key bound. */
    setErrorCodeAbort(4281);
    return -1;
  }

  for (j= 0; j<key_count; j++)
  {
    Uint32 bound_type;
    /* If key is part of lower bound */
    if (bound.low_key && j<bound.low_key_count)
    {
      /* Inclusive if defined, or matching rows can include this value */
      bound_type= bound.low_inclusive  || j+1 < bound.low_key_count ?
        BoundLE : BoundLT;
      ndbrecord_insert_bound(key_record, key_record->key_indexes[j],
                             bound.low_key, bound_type);
    }
    /* If key is part of upper bound */
    if (bound.high_key && j<bound.high_key_count)
    {
      /* Inclusive if defined, or matching rows can include this value */
      bound_type= bound.high_inclusive  || j+1 < bound.high_key_count ?
        BoundGE : BoundGT;
      ndbrecord_insert_bound(key_record, key_record->key_indexes[j],
                             bound.high_key, bound_type);
    }
  }

  /* Set the length of this bound
   * Length = bound end - bound start
   * Pack into Uint32 with range no and bound type as described 
   * in KeyInfo.hpp
   */
  bound_head= *m_first_bound_word;
  bound_head|=
    (theTupKeyLen - m_this_bound_start) << 16 | (range_no << 4);
  *m_first_bound_word= bound_head;
  m_first_bound_word= theKEYINFOptr + theTotalNrOfKeyWordInSignal;
  m_this_bound_start= theTupKeyLen;

  /*
    Now check if the range bounds a single distribution key. If so, we need
    scan only a single fragment.
    
    ToDo: we do not attempt to identify the case where we have multiple
    ranges, but they all bound the same single distribution key. It seems
    not really worth the effort to optimise this case, better to fix the
    multi-range protocol so that the distribution key could be specified
    individually for each of the multiple ranges.
  */
  if (m_num_bounds == 1 &&  
      ! theDistrKeyIndicator_ && // Partitioning not already specified.
      ! m_multi_range)           // Only single range optimisation currently
  {
    Uint32 index_distkeys = key_record->m_no_of_distribution_keys;
    Uint32 table_distkeys = m_attribute_record->m_no_of_distribution_keys;
    Uint32 distkey_min= key_record->m_min_distkey_prefix_length;
    if (index_distkeys == table_distkeys &&
        common_key_count >= distkey_min &&
        bound.low_key &&
        bound.high_key &&
        0==compare_index_row_prefix(key_record,
                                    bound.low_key,
                                    bound.high_key,
                                    distkey_min))
      setDistKeyFromRange(key_record, m_attribute_record,
                          bound.low_key, distkey_min);
  }
  return 0;
} // ::setBound();


int
NdbIndexScanOperation::scanIndexImpl(const NdbRecord *key_record,
                                     const NdbRecord *result_record,
                                     NdbOperation::LockMode lock_mode,
                                     const unsigned char *result_mask,
                                     const NdbIndexScanOperation::IndexBound *bound,
                                     const NdbScanOperation::ScanOptions *options,
                                     Uint32 sizeOfOptions)
{
  const NdbTableImpl *table_impl;       // The table schema object
  NdbBlob *lastBlob;
  int res;
  Uint32 i;
  Uint32 column_count;
  bool haveBlob= false;
  Uint32 scan_flags = 0;
  Uint32 parallel = 0;
  Uint32 batch = 0;

  if (options != NULL)
  {
    /* Check options size for versioning... */
    if (unlikely((sizeOfOptions !=0) &&
                 (sizeOfOptions != sizeof(ScanOptions))))
    {
      /* Handle different sized ScanOptions
       * Probably smaller is old version, larger is new version
       */
      
      /* No other versions supported currently */
      setErrorCodeAbort(4298);
      /* Invalid or unsupported ScanOptions structure */
      return -1;
    }
    
    /* Process some initial ScanOptions here
     * The rest will be handled later
     */
    if (options->optionsPresent & ScanOptions::SO_SCANFLAGS)
      scan_flags = options->scan_flags;
    if (options->optionsPresent & ScanOptions::SO_PARALLEL)
      parallel = options->parallel;
    if (options->optionsPresent & ScanOptions::SO_BATCH)
      batch = options->batch;
  }

  if (!(key_record->flags & NdbRecord::RecHasAllKeys))
  {
    setErrorCodeAbort(4292);
    return -1;
  }

  if (scan_flags & NdbScanOperation::SF_OrderBy)
  {
    /**
     * For ordering, we need all keys in the result row.
     *
     * So for each key column, check that it is included in the result
     * NdbRecord.
     */
    for (i = 0; i < key_record->key_index_length; i++)
    {
      const NdbRecord::Attr *key_col =
        &key_record->columns[key_record->key_indexes[i]];
      if (key_col->attrId >= result_record->m_attrId_indexes_length ||
          result_record->m_attrId_indexes[key_col->attrId] < 0)
      {
        setErrorCodeAbort(4292);
        return -1;
      }
    }
  }

  if (!(key_record->flags & NdbRecord::RecIsIndex))
  {
    setErrorCodeAbort(4283);
    return -1;
  }
  if (result_record->flags & NdbRecord::RecIsIndex)
  {
    setErrorCodeAbort(4340);
    return -1;
  }

  table_impl= result_record->table;
  m_type= NdbOperation::OrderedIndexScan;
  m_currentTable= table_impl;

  m_key_record = key_record;
  m_attribute_record= result_record; // Mark using NdbRecord for processIndexScanDefs
  res= processIndexScanDefs(lock_mode, scan_flags, parallel, batch);
  if (res==-1)
    return -1;

  result_record->copyMask(m_read_mask, result_mask);

  /* Fix theStatus as set in processIndexScanDefs(). */
  theStatus= NdbOperation::UseNdbRecord;

  lastBlob= NULL;
  column_count= 0;
  for (i= 0; i<result_record->noOfColumns; i++)
  {
    const NdbRecord::Attr *col;
    Uint32 ah;
    Uint32 attrId;

    col= &result_record->columns[i];

    /*
      Skip column if result_mask says so.
      But cannot mask pseudo columns, nor key columns in ordered scans.
    */
    attrId= col->attrId;
    if ( result_mask &&
         !(attrId & AttributeHeader::PSEUDO) &&
         !( (scan_flags & NdbScanOperation::SF_OrderBy) &&
            (col->flags & NdbRecord::IsKey) ) &&
         !(result_mask[attrId>>3] & (1<<(attrId & 7))) )
    {
      continue;
    }

    /* Create blob handle for any blob column. */
    if (unlikely(col->flags & NdbRecord::IsBlob))
    {
      m_keyInfo= 1;          // Need keyinfo for blob scan
      haveBlob= true;
      continue;
    }

    AttributeHeader::init(&ah, attrId, 0);
    res= insertATTRINFO(ah);
    if (res==-1)
      return -1;

    if (col->flags & NdbRecord::IsDisk)
      m_no_disk_flag= false;
    column_count++;
  }
  theReceiver.m_record.m_column_count= column_count;

  theInitialReadSize= theTotalCurrAI_Len - AttrInfo::SectionSizeInfoLength;

  /* Handle any getValue() calls made against the old API. */
  if (m_scanUsingOldApi)
  {
    if (handleScanGetValuesOldApi() !=0)
      return -1;
  }

  /* Handle remaining scan options and other extras
   * Always called for old style API 
   */
  if (options != NULL)
  {
    if (handleScanOptions(options) != 0)
      return -1;
  }

  /*
   * Set up first key bound, if present
   * Extra bounds (MRR) can be added later
   */
  if (bound != NULL)
  {
    setBound(key_record, *bound);
  }

  /* Get Blob handles for this scan unless it's using the
   * old Api.  Old Api scans set up Blob values in the
   * getBlobHandle() call
   */
  if (unlikely(haveBlob) && !m_scanUsingOldApi)
  {
    if (getBlobHandlesNdbRecord(m_transConnection) == -1)
      return -1;
  }

  /* Add interpreted code words to ATTRINFO signal
   * chain as necessary
   */
  if (m_interpreted_code != NULL)
  {
    if (addInterpretedCode(theNdbCon->theTCConPtr,
                           theNdbCon->theTransactionId) == -1)
      return -1;
  }

  /* Scan is now mostly defined, so let's start preparing
   * the signals and the receiver.
   * Extra signals can be linked in when :
   *  - Bounds are added
   */
  if (prepareSendScan(theNdbCon->theTCConPtr, 
                      theNdbCon->theTransactionId) == -1)
    /* Error code should be set */
    return -1;

  return 0;
} // ::scanIndexImpl();


/* readTuples() method for table scans
 * This method performs minimal validation and initialisation,
 * deferring most of the work to a later call to processTableScanDefs
 * below.
 */
int 
NdbScanOperation::readTuples(NdbScanOperation::LockMode lm,
                             Uint32 scan_flags, 
                             Uint32 parallel,
                             Uint32 batch)
{
  // It is only possible to call readTuples if 
  //  1. the scan transaction doesn't already  contain another scan operation
  //  2. We have not already defined an old Api scan operation.
  if (theNdbCon->theScanningOp != NULL ||
      m_scanUsingOldApi ){
    setErrorCode(4605);
    return -1;
  }

  m_scanUsingOldApi= true;
  /* Save parameters for later */
  m_savedLockModeOldApi= lm;
  m_savedScanFlagsOldApi= scan_flags;
  m_savedParallelOldApi= parallel;
  m_savedBatchOldApi= batch;

  return 0;
};

/* Most of the scan definition work for old + NdbRecord API scans is done here */
int 
NdbScanOperation::processTableScanDefs(NdbScanOperation::LockMode lm,
                                       Uint32 scan_flags, 
                                       Uint32 parallel,
                                       Uint32 batch)
{
  m_ordered = m_descending = false;
  Uint32 fragCount = m_currentTable->m_fragmentCount;

  if (parallel > fragCount || parallel == 0) {
     parallel = fragCount;
  }

  theNdbCon->theScanningOp = this;
  bool tupScan = (scan_flags & SF_TupScan);

#if 0 // XXX temp for testing
  { char* p = getenv("NDB_USE_TUPSCAN");
    if (p != 0) {
      unsigned n = atoi(p); // 0-10
      if ((unsigned int) (::time(0) % 10) < n) tupScan = true;
    }
  }
#endif
  if (scan_flags & SF_DiskScan)
  {
    tupScan = true;
    m_no_disk_flag = false;
  }
  
  bool rangeScan= false;

  /* NdbRecord defined scan, handle IndexScan specifics */
  if ( (int) m_accessTable->m_indexType ==
       (int) NdbDictionary::Index::OrderedIndex)
  {
    if (m_currentTable == m_accessTable){
      // Old way of scanning indexes, should not be allowed
      m_currentTable = theNdb->theDictionary->
        getTable(m_currentTable->m_primaryTable.c_str());
      assert(m_currentTable != NULL);
    }
    assert (m_currentTable != m_accessTable);
    // Modify operation state
    theStatus = GetValue;
    theOperationType  = OpenRangeScanRequest;
    rangeScan = true;
    tupScan = false;
  }
  
  if (rangeScan && (scan_flags & SF_OrderBy))
    parallel = fragCount; // Note we assume fragcount of base table==
                          // fragcount of index.
  
  theParallelism = parallel;    
  
  if(fix_receivers(parallel) == -1){
    setErrorCodeAbort(4000);
    return -1;
  }
  
  theSCAN_TABREQ = (!theSCAN_TABREQ ? theNdb->getSignal() : theSCAN_TABREQ);
  if (theSCAN_TABREQ == NULL) {
    setErrorCodeAbort(4000);
    return -1;
  }//if
  
  theSCAN_TABREQ->setSignal(GSN_SCAN_TABREQ);
  ScanTabReq * req = CAST_PTR(ScanTabReq, theSCAN_TABREQ->getDataPtrSend());
  req->apiConnectPtr = theNdbCon->theTCConPtr;
  req->tableId = m_accessTable->m_id;
  req->tableSchemaVersion = m_accessTable->m_version;
  req->storedProcId = 0xFFFF;
  req->buddyConPtr = theNdbCon->theBuddyConPtr;
  req->first_batch_size = batch; // Save user specified batch size
  
  Uint32 reqInfo = 0;
  ScanTabReq::setParallelism(reqInfo, parallel);
  ScanTabReq::setScanBatch(reqInfo, 0);
  ScanTabReq::setRangeScanFlag(reqInfo, rangeScan);
  ScanTabReq::setTupScanFlag(reqInfo, tupScan);
  req->requestInfo = reqInfo;

  m_keyInfo = (scan_flags & SF_KeyInfo) ? 1 : 0;
  setReadLockMode(lm);

  Uint64 transId = theNdbCon->getTransactionId();
  req->transId1 = (Uint32) transId;
  req->transId2 = (Uint32) (transId >> 32);

  NdbApiSignal* tSignal = theSCAN_TABREQ->next();
  if(!tSignal)
  {
    theSCAN_TABREQ->next(tSignal = theNdb->getSignal());
  }
  theLastKEYINFO = tSignal;
  
  tSignal->setSignal(GSN_KEYINFO);
  theKEYINFOptr = ((KeyInfo*)tSignal->getDataPtrSend())->keyData;
  theTotalNrOfKeyWordInSignal= 0;

  getFirstATTRINFOScan();
  return 0;
}

int
NdbScanOperation::setInterpretedCode(const NdbInterpretedCode *code)
{
  if (theStatus == NdbOperation::UseNdbRecord)
  {
    setErrorCodeAbort(4284); // Cannot mix NdbRecAttr and NdbRecord methods...
    return -1;
  }

  if ((code->m_flags & NdbInterpretedCode::Finalised) == 0)
  {
    setErrorCodeAbort(4519); //  NdbInterpretedCode::finalise() not called.
    return -1;
  }

  m_interpreted_code= code;
  
  return 0;
}

NdbInterpretedCode*
NdbScanOperation::allocInterpretedCodeOldApi()
{
  /* Should only be called once */
  assert (m_interpretedCodeOldApi == NULL);

  /* Old Api scans only */
  if (! m_scanUsingOldApi)
  {
    /* NdbScanFilter constructor taking NdbOperation is not 
     * supported for NdbRecord
     */
    setErrorCodeAbort(4536);
    return NULL;
  }

  m_interpretedCodeOldApi = new NdbInterpretedCode(m_currentTable->m_facade);

  if (m_interpretedCodeOldApi == NULL)
    setErrorCodeAbort(4000); // Memory allocation error

  return m_interpretedCodeOldApi;
}

void
NdbScanOperation::freeInterpretedCodeOldApi()
{
  if (m_interpretedCodeOldApi != NULL)
  {
    delete m_interpretedCodeOldApi;
    m_interpretedCodeOldApi= NULL;
  }
}


void
NdbScanOperation::setReadLockMode(LockMode lockMode)
{
  bool lockExcl, lockHoldMode, readCommitted;
  switch (lockMode)
  {
    case LM_CommittedRead:
      lockExcl= false;
      lockHoldMode= false;
      readCommitted= true;
      break;
    case LM_SimpleRead:
    case LM_Read:
      lockExcl= false;
      lockHoldMode= true;
      readCommitted= false;
      break;
    case LM_Exclusive:
      lockExcl= true;
      lockHoldMode= true;
      readCommitted= false;
      m_keyInfo= 1;
      break;
    default:
      /* Not supported / invalid. */
      assert(false);
  }
  theLockMode= lockMode;
  ScanTabReq *req= CAST_PTR(ScanTabReq, theSCAN_TABREQ->getDataPtrSend());
  Uint32 reqInfo= req->requestInfo;
  ScanTabReq::setLockMode(reqInfo, lockExcl);
  ScanTabReq::setHoldLockFlag(reqInfo, lockHoldMode);
  ScanTabReq::setReadCommittedFlag(reqInfo, readCommitted);
  req->requestInfo= reqInfo;
}

int
NdbScanOperation::fix_receivers(Uint32 parallel){
  assert(parallel > 0);
  if(parallel > m_allocated_receivers){
    const Uint32 sz = parallel * (4*sizeof(char*)+sizeof(Uint32));

    /* Allocate as Uint64 to ensure proper alignment for pointers. */
    Uint64 * tmp = new Uint64[(sz+7)/8];
    if (tmp == NULL)
    {
      setErrorCodeAbort(4000);
      return -1;
    }
    // Save old receivers
    memcpy(tmp, m_receivers, m_allocated_receivers*sizeof(char*));
    delete[] m_array;
    m_array = (Uint32*)tmp;
    
    m_receivers = (NdbReceiver**)tmp;
    m_api_receivers = m_receivers + parallel;
    m_conf_receivers = m_api_receivers + parallel;
    m_sent_receivers = m_conf_receivers + parallel;
    m_prepared_receivers = (Uint32*)(m_sent_receivers + parallel);

    // Only get/init "new" receivers
    NdbReceiver* tScanRec;
    for (Uint32 i = m_allocated_receivers; i < parallel; i ++) {
      tScanRec = theNdb->getNdbScanRec();
      if (tScanRec == NULL) {
        setErrorCodeAbort(4000);
        return -1;
      }//if
      m_receivers[i] = tScanRec;
      tScanRec->init(NdbReceiver::NDB_SCANRECEIVER, false, this);
    }
    m_allocated_receivers = parallel;
  }
  
  reset_receivers(parallel, 0);
  return 0;
}

/**
 * Move receiver from send array to conf:ed array
 */
void
NdbScanOperation::receiver_delivered(NdbReceiver* tRec){
  if(theError.code == 0){
    if(DEBUG_NEXT_RESULT)
      ndbout_c("receiver_delivered");
    
    Uint32 idx = tRec->m_list_index;
    Uint32 last = m_sent_receivers_count - 1;
    if(idx != last){
      NdbReceiver * move = m_sent_receivers[last];
      m_sent_receivers[idx] = move;
      move->m_list_index = idx;
    }
    m_sent_receivers_count = last;
    
    last = m_conf_receivers_count;
    m_conf_receivers[last] = tRec;
    m_conf_receivers_count = last + 1;
    tRec->m_current_row = 0;
  }
}

/**
 * Remove receiver as it's completed
 */
void
NdbScanOperation::receiver_completed(NdbReceiver* tRec){
  if(theError.code == 0){
    if(DEBUG_NEXT_RESULT)
      ndbout_c("receiver_completed");
    
    Uint32 idx = tRec->m_list_index;
    Uint32 last = m_sent_receivers_count - 1;
    if(idx != last){
      NdbReceiver * move = m_sent_receivers[last];
      m_sent_receivers[idx] = move;
      move->m_list_index = idx;
    }
    m_sent_receivers_count = last;
  }
}

/*****************************************************************************
 * int getFirstATTRINFOScan( U_int32 aData )
 *
 * Return Value:  Return 0:   Successful
 *                Return -1:  All other cases
 * Parameters:    None:            Only allocate the first signal.
 * Remark:        When a scan is defined we need to use this method instead 
 *                of insertATTRINFO for the first signal. 
 *                This is because we need not to mess up the code in 
 *                insertATTRINFO with if statements since we are not 
 *                interested in the TCKEYREQ signal.
 *****************************************************************************/
int
NdbScanOperation::getFirstATTRINFOScan()
{
  NdbApiSignal* tSignal;

  tSignal = theNdb->getSignal();
  if (tSignal == NULL){
    setErrorCodeAbort(4000);      
    return -1;    
  }
  tSignal->setSignal(m_attrInfoGSN);

  theAI_LenInCurrAI = AttrInfo::HeaderLength + AttrInfo::SectionSizeInfoLength;
  theATTRINFOptr = &tSignal->getDataPtrSend()[8];
  theFirstATTRINFO = tSignal;
  theCurrentATTRINFO = tSignal;
  theCurrentATTRINFO->next(NULL);

  return 0;
}

int
NdbScanOperation::executeCursor(int nodeId)
{
  /*
   * Call finaliseScanOldApi() for old style scans before
   * proceeding
   */  
  if (m_scanUsingOldApi &&
      finaliseScanOldApi() == -1) 
    return -1;

  NdbTransaction * tCon = theNdbCon;
  TransporterFacade* tp = theNdb->theImpl->m_transporter_facade;
  Guard guard(tp->theMutexPtr);

  Uint32 seq = tCon->theNodeSequence;

  if (tp->get_node_alive(nodeId) &&
      (tp->getNodeSequence(nodeId) == seq)) {

    tCon->theMagicNumber = 0x37412619;

    if (doSendScan(nodeId) == -1)
      return -1;

    m_executed= true; // Mark operation as executed
    return 0;
  } else {
    if (!(tp->get_node_stopping(nodeId) &&
          (tp->getNodeSequence(nodeId) == seq))){
      TRACE_DEBUG("The node is hard dead when attempting to start a scan");
      setErrorCode(4029);
      tCon->theReleaseOnClose = true;
    } else {
      TRACE_DEBUG("The node is stopping when attempting to start a scan");
      setErrorCode(4030);
    }//if
    tCon->theCommitStatus = NdbTransaction::Aborted;
  }//if
  return -1;
}


int 
NdbScanOperation::nextResult(bool fetchAllowed, bool forceSend)
{
  /* Defer to NdbRecord implementation, which will copy values
   * out into the user's RecAttr objects.
   */
  const char * dummyOutRowPtr;

  if (unlikely(! m_scanUsingOldApi))
  {
    /* Cannot mix NdbRecAttr and NdbRecord methods in one operation */
    setErrorCode(4284);
    return -1;
  }
    

  return nextResult(&dummyOutRowPtr,
                    fetchAllowed,
                    forceSend);
};

/* nextResult() for NdbRecord operation. */
int
NdbScanOperation::nextResult(const char ** out_row_ptr,
                             bool fetchAllowed, bool forceSend)
{
  int res;

  if ((res = nextResultNdbRecord(*out_row_ptr, fetchAllowed, forceSend)) == 0) {
    NdbBlob* tBlob= theBlobList;
    NdbRecAttr *getvalue_recattr= theReceiver.theFirstRecAttr;
    if (((UintPtr)tBlob | (UintPtr)getvalue_recattr) != 0)
    {
      Uint32 idx= m_current_api_receiver;
      assert(idx < m_api_receivers_count);
      const NdbReceiver *receiver= m_api_receivers[m_current_api_receiver];
      Uint32 pos= 0;

      /* First take care of any getValue(). */
      while (getvalue_recattr != NULL)
      {
        const char *attr_data;
        Uint32 attr_size;
        if (receiver->getScanAttrData(attr_data, attr_size, pos) == -1)
          return -1;
        if (!getvalue_recattr->receive_data((const Uint32 *)attr_data,
                                            attr_size))
          return -1;                            // purecov: deadcode
        getvalue_recattr= getvalue_recattr->next();
      }

      /* Handle blobs. */
      if (tBlob)
      {
        Uint32 infoword;                          // Not used for blobs
        Uint32 key_length;
        const char *key_data;
        res= receiver->get_keyinfo20(infoword, key_length, key_data);
        if (res == -1)
          return -1;

        do
        {
          if (tBlob->atNextResultNdbRecord(key_data, key_length*4) == -1)
            return -1;
          tBlob= tBlob->theNext;
        } while (tBlob != 0);
        /* Flush blob part ops on behalf of user. */
        if (m_transConnection->executePendingBlobOps() == -1)
          return -1;
      }
    }
    return 0;
  }
  return res;
}

int
NdbScanOperation::nextResultNdbRecord(const char * & out_row,
                                      bool fetchAllowed, bool forceSend)
{
  if (m_ordered)
    return ((NdbIndexScanOperation*)this)->next_result_ordered_ndbrecord
      (out_row, fetchAllowed, forceSend);

  /* Return a row immediately if any is available. */
  while (m_current_api_receiver < m_api_receivers_count)
  {
    NdbReceiver *tRec= m_api_receivers[m_current_api_receiver];
    if (tRec->nextResult())
    {
      out_row= tRec->get_row();
      return 0;
    }
    m_current_api_receiver++;
  }

  if (!fetchAllowed)
  {
    /*
      Application wants to be informed that no more rows are available
      immediately.
    */
    return 2;
  }

  /* Now we have to wait for more rows (or end-of-file on all receivers). */
  Uint32 nodeId = theNdbCon->theDBnode;
  TransporterFacade* tp = theNdb->theImpl->m_transporter_facade;
  int retVal= 2;
  Uint32 idx, last;
  /*
    The rest needs to be done under mutex due to synchronization with receiver
    thread.
  */
  PollGuard poll_guard(tp, &theNdb->theImpl->theWaiter,
                       theNdb->theNdbBlockNumber);

  const Uint32 seq= theNdbCon->theNodeSequence;

  if(theError.code)
  {
    goto err4;
  }

  if(seq == tp->getNodeSequence(nodeId) &&
     send_next_scan(m_current_api_receiver, false) == 0)
  {
    idx= m_current_api_receiver;
    last= m_api_receivers_count;
    Uint32 timeout= tp->m_waitfor_timeout;

    do {
      if (theError.code){
        setErrorCode(theError.code);
        return -1;
      }

      Uint32 cnt= m_conf_receivers_count;
      Uint32 sent= m_sent_receivers_count;

      if (cnt > 0)
      {
        /* New receivers with completed batches available. */
        memcpy(m_api_receivers+last, m_conf_receivers, cnt * sizeof(char*));
        last+= cnt;
        m_conf_receivers_count= 0;
      }
      else if (retVal == 2 && sent > 0)
      {
        /* No completed... */
        int ret_code= poll_guard.wait_scan(3*timeout, nodeId, forceSend);
        if (ret_code == 0 && seq == tp->getNodeSequence(nodeId)) {
          continue;
        } else if(ret_code == -1){
          retVal= -1;
        } else {
          idx= last;
          retVal= -2; //return_code;
        }
      }
      else if (retVal == 2)
      {
        /**
         * No completed & no sent -> EndOfData
         */
        theError.code= -1; // make sure user gets error if he tries again
        return 1;
      }

      if (retVal == 0)
        break;

      while (idx < last)
      {
        NdbReceiver* tRec= m_api_receivers[idx];
        if (tRec->nextResult())
        {
          out_row= tRec->get_row();
          retVal= 0;
          break;
        }
        idx++;
      }
    } while(retVal == 2);
  } else {
    retVal = -3;
  }

  m_api_receivers_count= last;
  m_current_api_receiver= idx;

  switch(retVal)
  {
  case 0:
  case 1:
  case 2:
    return retVal;
  case -1:
    setErrorCode(4008); // Timeout
    break;
  case -2:
    setErrorCode(4028); // Node fail
    break;
  case -3: // send_next_scan -> return fail (set error-code self)
    if(theError.code == 0)
      setErrorCode(4028); // seq changed = Node fail
    break;
  case -4:
err4:
    setErrorCode(theError.code);
    break;
  }

  theNdbCon->theTransactionIsStarted= false;
  theNdbCon->theReleaseOnClose= true;
  return -1;
}

int
NdbScanOperation::send_next_scan(Uint32 cnt, bool stopScanFlag)
{
  if(cnt > 0){
    NdbApiSignal tSignal(theNdb->theMyRef);
    tSignal.setSignal(GSN_SCAN_NEXTREQ);
    
    Uint32* theData = tSignal.getDataPtrSend();
    theData[0] = theNdbCon->theTCConPtr;
    theData[1] = stopScanFlag == true ? 1 : 0;
    Uint64 transId = theNdbCon->theTransactionId;
    theData[2] = transId;
    theData[3] = (Uint32) (transId >> 32);
    
    /**
     * Prepare ops
     */
    Uint32 last = m_sent_receivers_count;
    Uint32 * prep_array = (cnt > 21 ? m_prepared_receivers : theData + 4);
    Uint32 sent = 0;
    for(Uint32 i = 0; i<cnt; i++){
      NdbReceiver * tRec = m_api_receivers[i];
      if((prep_array[sent] = tRec->m_tcPtrI) != RNIL)
      {
        m_sent_receivers[last+sent] = tRec;
        tRec->m_list_index = last+sent;
        tRec->prepareSend();
        sent++;
      }
    }
    memmove(m_api_receivers, m_api_receivers+cnt, 
            (theParallelism-cnt) * sizeof(char*));
    
    int ret = 0;
    if(sent)
    {
      Uint32 nodeId = theNdbCon->theDBnode;
      TransporterFacade * tp = theNdb->theImpl->m_transporter_facade;
      if(cnt > 21){
        tSignal.setLength(4);
        LinearSectionPtr ptr[3];
        ptr[0].p = prep_array;
        ptr[0].sz = sent;
        ret = tp->sendSignal(&tSignal, nodeId, ptr, 1);
      } else {
        tSignal.setLength(4+sent);
        ret = tp->sendSignal(&tSignal, nodeId);
      }
    }
    m_sent_receivers_count = last + sent;
    m_api_receivers_count -= cnt;
    m_current_api_receiver = 0;
    
    return ret;
  }
  return 0;
}

int 
NdbScanOperation::prepareSend(Uint32  TC_ConnectPtr, Uint64  TransactionId)
{
  abort();
  return 0;
}

int 
NdbScanOperation::doSend(int ProcessorId)
{
  return 0;
}

void NdbScanOperation::close(bool forceSend, bool releaseOp)
{
  DBUG_ENTER("NdbScanOperation::close");
  DBUG_PRINT("enter", ("this: 0x%lx  tcon: 0x%lx  con: 0x%lx  force: %d  release: %d",
                       (long) this,
                       (long) m_transConnection, (long) theNdbCon,
                       forceSend, releaseOp));

  if(m_transConnection){
    if(DEBUG_NEXT_RESULT)
      ndbout_c("close() theError.code = %d "
               "m_api_receivers_count = %d "
               "m_conf_receivers_count = %d "
               "m_sent_receivers_count = %d",
               theError.code, 
               m_api_receivers_count,
               m_conf_receivers_count,
               m_sent_receivers_count);
    
    TransporterFacade* tp = theNdb->theImpl->m_transporter_facade;
    /*
      The PollGuard has an implicit call of unlock_and_signal through the
      ~PollGuard method. This method is called implicitly by the compiler
      in all places where the object is out of context due to a return,
      break, continue or simply end of statement block
    */
    PollGuard poll_guard(tp, &theNdb->theImpl->theWaiter,
                         theNdb->theNdbBlockNumber);
    close_impl(tp, forceSend, &poll_guard);
  }

  NdbConnection* tCon = theNdbCon;
  NdbConnection* tTransCon = m_transConnection;
  theNdbCon = NULL;
  m_transConnection = NULL;

  if (tTransCon && releaseOp) 
  {
    NdbIndexScanOperation* tOp = (NdbIndexScanOperation*)this;

    bool ret = true;
    if (theStatus != WaitResponse)
    {
      /**
       * Not executed yet
       */
      ret = 
        tTransCon->releaseScanOperation(&tTransCon->m_theFirstScanOperation,
                                        &tTransCon->m_theLastScanOperation,
                                        tOp);
    }
    else
    {
      ret = tTransCon->releaseScanOperation(&tTransCon->m_firstExecutedScanOp,
                                            0, tOp);
    }
    assert(ret);
  }
  
  tCon->theScanningOp = 0;
  theNdb->closeTransaction(tCon);
  theNdb->theRemainingStartTransactions--;
  DBUG_VOID_RETURN;
}

void
NdbScanOperation::execCLOSE_SCAN_REP(){
  m_conf_receivers_count = 0;
  m_sent_receivers_count = 0;
}

void NdbScanOperation::release()
{
  if(theNdbCon != 0 || m_transConnection != 0){
    close();
  }
  for(Uint32 i = 0; i<m_allocated_receivers; i++){
    m_receivers[i]->release();
  }
  if (m_scan_buffer)
  {
    delete[] m_scan_buffer;
    m_scan_buffer= NULL;
  }

  NdbOperation::release();
  
  if(theSCAN_TABREQ)
  {
    theNdb->releaseSignal(theSCAN_TABREQ);
    theSCAN_TABREQ = 0;
  }
}

/*
 * This method finalises an Old API defined scan
 * This is done just prior to scan execution
 * The parameters provided via the RecAttr scan interface are
 * used to create an NdbRecord based scan
 */
int NdbScanOperation::finaliseScanOldApi()
{
  /* For a scan we use an NdbRecord structure for this
   * table, and add the user-requested values in a similar
   * way to the extra GetValues mechanism
   */
  assert(theOperationType == OpenScanRequest |
         theOperationType == OpenRangeScanRequest);

  /* Prepare ScanOptions structure using saved parameters */
  ScanOptions options;
  options.optionsPresent=(ScanOptions::SO_SCANFLAGS |
                          ScanOptions::SO_PARALLEL |
                          ScanOptions::SO_BATCH);

  options.scan_flags= m_savedScanFlagsOldApi;
  options.parallel= m_savedParallelOldApi;
  options.batch= m_savedBatchOldApi;

  /* customData, interpretedCode or partitionId should 
   * already be set in the operation members - no need 
   * to pass in as ScanOptions
   */

  /* Next, call scanTable, passing in some of the 
   * parameters we saved
   * It will look after building the correct signals
   */
  int result= -1;

  if (theOperationType == OpenScanRequest)
    /* Create table scan operation with an empty
     * mask for NdbRecord values
     */
    result= scanTableImpl(m_currentTable->m_ndbrecord,
                          m_savedLockModeOldApi,
                          m_currentTable->m_emptyMask,
                          &options,
                          sizeof(ScanOptions));
  else
  {
    assert(theOperationType == OpenRangeScanRequest);
    NdbIndexScanOperation *isop = 
      reinterpret_cast<NdbIndexScanOperation*>(this);

    /* Prepare a single bound if necessary */
    NdbIndexScanOperation::IndexBound ib;
    if (isop->buildIndexBoundOldApi(ib) != 0)
      return -1;

    /* If this is an ordered scan, then we need
     * the pk columns in the mask, otherwise we
     * don't
     */
    const unsigned char * resultMask= 
      ((m_savedScanFlagsOldApi & SF_OrderBy) !=0) ? 
      m_currentTable->m_pkMask : 
      m_currentTable->m_emptyMask;

    result= isop->scanIndexImpl(m_accessTable->m_ndbrecord,
                                m_currentTable->m_ndbrecord,
                                m_savedLockModeOldApi,
                                resultMask,
                                &ib,
                                &options,
                                sizeof(ScanOptions));

    isop->releaseIndexBoundOldApi();
  }

  /* Free any scan-owned ScanFilter generated InterpretedCode
   * object
   */
  freeInterpretedCodeOldApi();

  return result;
}

/***************************************************************************
int prepareSendScan(Uint32 aTC_ConnectPtr,
                    Uint64 aTransactionId)

Return Value:   Return 0 : preparation of send was succesful.
                Return -1: In all other case.   
Parameters:     aTC_ConnectPtr: the Connect pointer to TC.
                aTransactionId: the Transaction identity of the transaction.
Remark:         Puts the the final data into ATTRINFO signal(s)  after this 
                we know the how many signal to send and their sizes
***************************************************************************/
int NdbScanOperation::prepareSendScan(Uint32 aTC_ConnectPtr,
                                      Uint64 aTransactionId){
  if (theInterpretIndicator != 1 ||
      (theOperationType != OpenScanRequest &&
       theOperationType != OpenRangeScanRequest)) {
    setErrorCodeAbort(4005);
    return -1;
  }

  theErrorLine = 0;

  /* All scans use NdbRecord at this stage */
  assert(m_attribute_record);

  /* Some signal building code sets all intermediate ATTRINFOs to
   * max length, and it's this line's job to set the correct
   * value for the last signal
   */
  theCurrentATTRINFO->setLength(theAI_LenInCurrAI);

  /**
   * Prepare all receivers
   */
  theReceiver.prepareSend();
  bool keyInfo = m_keyInfo;
  Uint32 key_size= keyInfo ? m_attribute_record->m_keyLenInWords : 0;

  /**
   * The number of records sent by each LQH is calculated and the kernel
   * is informed of this number by updating the SCAN_TABREQ signal
   */
  ScanTabReq * req = CAST_PTR(ScanTabReq, theSCAN_TABREQ->getDataPtrSend());
  Uint32 batch_size = req->first_batch_size; // User specified
  Uint32 batch_byte_size, first_batch_size;
  theReceiver.calculate_batch_size(key_size,
                                   theParallelism,
                                   batch_size,
                                   batch_byte_size,
                                   first_batch_size,
                                   m_attribute_record);
  ScanTabReq::setScanBatch(req->requestInfo, batch_size);
  req->batch_byte_size= batch_byte_size;
  req->first_batch_size= first_batch_size;

  /**
   * Set keyinfo, nodisk and distribution key flags in 
   * ScanTabReq
   *  (Always keyinfo when using blobs)
   */
  Uint32 reqInfo = req->requestInfo;
  ScanTabReq::setKeyinfoFlag(reqInfo, keyInfo);
  ScanTabReq::setNoDiskFlag(reqInfo, m_no_disk_flag);

  /* Set distribution key info if required */
  ScanTabReq::setDistributionKeyFlag(reqInfo, theDistrKeyIndicator_);
  req->requestInfo = reqInfo;
  req->distributionKey= theDistributionKey;

  theSCAN_TABREQ->setLength(ScanTabReq::StaticLength + theDistrKeyIndicator_);

  /* All scans use NdbRecord internally */
  assert(theStatus == UseNdbRecord);
  
  Uint32 extra_size= 0;
  if (unlikely(theReceiver.theFirstRecAttr != NULL))
    extra_size= calcGetValueSize();
  
  assert(theParallelism > 0);
  Uint32 rowsize= m_receivers[0]->ndbrecord_rowsize(m_attribute_record,
                                                    key_size,
                                                    m_read_range_no,
                                                    extra_size);
  Uint32 bufsize= batch_size*rowsize;
  char *buf= new char[bufsize*theParallelism];
  if (!buf)
  {
    setErrorCodeAbort(4000); // "Memory allocation error"
    return -1;
  }
  assert(!m_scan_buffer);
  m_scan_buffer= buf;
  
  for (Uint32 i = 0; i<theParallelism; i++)
  {
    m_receivers[i]->do_setup_ndbrecord(m_attribute_record, batch_size,
                                       key_size, m_read_range_no,
                                       rowsize, buf,
                                       theReceiver.m_record.m_column_count);
    buf+= bufsize;
  }

  /* Update ATTRINFO section sizes info */
  if (doSendSetAISectionSizes() == -1)
    return -1;

  return 0;
}

int
NdbScanOperation::doSendSetAISectionSizes()
{
  // Set the scan AI section sizes.
  theFirstATTRINFO->setData(theInitialReadSize, 4);
  theFirstATTRINFO->setData(theInterpretedSize, 5);
  theFirstATTRINFO->setData(0, 6); // Update size
  theFirstATTRINFO->setData(0, 7); // Final read size
  theFirstATTRINFO->setData(theSubroutineSize, 8); 

  return 0;
}

/*
  Compute extra space needed to buffer getValue() results in NdbRecord
  scans.
 */
Uint32
NdbScanOperation::calcGetValueSize()
{
  Uint32 size= 0;
  const NdbRecAttr *ra= theReceiver.theFirstRecAttr;
  while (ra != NULL)
  {
    size+= sizeof(Uint32) + ra->getColumn()->getSizeInBytes();
    ra= ra->next();
  }
  return size;
}

/*****************************************************************************
int doSendScan()

Return Value:   Return >0 : send was succesful, returns number of signals sent
                Return -1: In all other case.   
Parameters:     aProcessorId: Receiving processor node
Remark:         Sends the ATTRINFO signal(s)
*****************************************************************************/
int
NdbScanOperation::doSendScan(int aProcessorId)
{
  Uint32 tSignalCount = 0;
  NdbApiSignal* tSignal;
 
  if (theInterpretIndicator != 1 ||
      (theOperationType != OpenScanRequest &&
       theOperationType != OpenRangeScanRequest)) {
      setErrorCodeAbort(4005);
      return -1;
  }
  
  assert(theSCAN_TABREQ != NULL);
  tSignal = theSCAN_TABREQ;
  
  Uint32 tupKeyLen = theTupKeyLen;
  Uint32 aTC_ConnectPtr = theNdbCon->theTCConPtr;
  Uint64 transId = theNdbCon->theTransactionId;
  
  /**
   * Update the "attribute info length in words" in SCAN_TABREQ before 
   * sending it. This could not be done before as it is possible to 
   * add bounds to an index scan after it is defined using 
   * scanIndex.
   */
  ScanTabReq * const req = CAST_PTR(ScanTabReq, tSignal->getDataPtrSend());
  if (unlikely(theTotalCurrAI_Len > ScanTabReq::MaxTotalAttrInfo)) {
    setErrorCode(4257);
    return -1;
  }
  req->attrLenKeyLen = (tupKeyLen << 16) | theTotalCurrAI_Len;

  TransporterFacade *tp = theNdb->theImpl->m_transporter_facade;
  LinearSectionPtr ptr[3];
  ptr[0].p = m_prepared_receivers;
  ptr[0].sz = theParallelism;
  if (tp->sendSignal(tSignal, aProcessorId, ptr, 1) == -1) {
    setErrorCode(4002);
    return -1;
  } 

  if (tupKeyLen > 0){
    // must have at least one signal since it contains attrLen for bounds
    assert(theLastKEYINFO != NULL);
    tSignal = theLastKEYINFO;
    tSignal->setLength(KeyInfo::HeaderLength + theTotalNrOfKeyWordInSignal);
    
    assert(theSCAN_TABREQ->next() != NULL);
    tSignal = theSCAN_TABREQ->next();
    
    NdbApiSignal* last;
    do {
      KeyInfo * keyInfo = CAST_PTR(KeyInfo, tSignal->getDataPtrSend());
      keyInfo->connectPtr = aTC_ConnectPtr;
      keyInfo->transId[0] = Uint32(transId);
      keyInfo->transId[1] = Uint32(transId >> 32);
      
      if (tp->sendSignal(tSignal,aProcessorId) == -1){
        setErrorCode(4002);
        return -1;
      }
      
      tSignalCount++;
      last = tSignal;
      tSignal = tSignal->next();
    } while(last != theLastKEYINFO);
  }
  
  tSignal = theFirstATTRINFO;
  while (tSignal != NULL) {
    AttrInfo * attrInfo = CAST_PTR(AttrInfo, tSignal->getDataPtrSend());
    attrInfo->connectPtr = aTC_ConnectPtr;
    attrInfo->transId[0] = Uint32(transId);
    attrInfo->transId[1] = Uint32(transId >> 32);
    
    if (tp->sendSignal(tSignal,aProcessorId) == -1){
      setErrorCode(4002);
      return -1;
    }
    tSignalCount++;
    tSignal = tSignal->next();
  }    
  theStatus = WaitResponse;  

  m_curr_row = 0;
  m_sent_receivers_count = theParallelism;
  if(m_ordered)
  {
    m_current_api_receiver = theParallelism;
    m_api_receivers_count = theParallelism;
  }
  
  return tSignalCount;
}//NdbOperation::doSendScan()


/* This method retrieves a pointer to the keyinfo for the current
 * row - it is used when creating a scan takeover operation
 */
int
NdbScanOperation::getKeyFromKEYINFO20(Uint32* data, Uint32 & size)
{
  NdbRecAttr * tRecAttr = m_curr_row;
  if(tRecAttr)
  {
    const Uint32 * src = (Uint32*)tRecAttr->aRef();

    assert(tRecAttr->get_size_in_bytes() > 0);
    assert(tRecAttr->get_size_in_bytes() < 65536);
    const Uint32 len = (tRecAttr->get_size_in_bytes() + 3)/4-1;

    assert(size >= len);
    memcpy(data, src, 4*len);
    size = len;
    return 0;
  }
  return -1;
}

/*****************************************************************************
 * NdbOperation* takeOverScanOp(NdbTransaction* updateTrans);
 *
 * Parameters:     The update transactions NdbTransaction pointer.
 * Return Value:   A reference to the transferred operation object 
 *                   or NULL if no success.
 * Remark:         Take over the scanning transactions NdbOperation 
 *                 object for a tuple to an update transaction, 
 *                 which is the last operation read in nextScanResult()
 *                 (theNdbCon->thePreviousScanRec)
 *
 *     FUTURE IMPLEMENTATION:   (This note was moved from header file.)
 *     In the future, it will even be possible to transfer 
 *     to a NdbTransaction on another Ndb-object.  
 *     In this case the receiving NdbTransaction-object must call 
 *     a method receiveOpFromScan to actually receive the information.  
 *     This means that the updating transactions can be placed
 *     in separate threads and thus increasing the parallelism during
 *     the scan process. 
 ****************************************************************************/
NdbOperation*
NdbScanOperation::takeOverScanOp(OperationType opType, NdbTransaction* pTrans)
{
  if (!m_scanUsingOldApi)
  {
    setErrorCodeAbort(4284);
    return NULL;
  }

  if (!m_keyInfo)
  {
    // Cannot take over lock if no keyinfo was requested
    setErrorCodeAbort(4604);
    return NULL;
  }

  /*
   * Get the Keyinfo from the NdbRecord result row
   */
  Uint32 infoword= 0;
  Uint32 len= 0;
  const Uint32 *src= NULL;

  Uint32 idx= m_current_api_receiver;
  if (idx >= m_api_receivers_count)
    return NULL;
  const NdbReceiver *receiver= m_api_receivers[m_current_api_receiver];

  /* Get this row's KeyInfo data */
  int res= receiver->get_keyinfo20(infoword, len, (const char*&) src);
  if (res == -1)
    return NULL;

  NdbOperation * newOp = pTrans->getNdbOperation(m_currentTable);
  if (newOp == NULL){
    return NULL;
  }
  pTrans->theSimpleState = 0;
    
  assert(len > 0);
  assert(len < 16384);

  newOp->theTupKeyLen = len;
  newOp->theOperationType = opType;
  newOp->m_abortOption = AbortOnError;
  switch (opType) {
  case (ReadRequest):
    newOp->theLockMode = theLockMode;
    // Fall through
  case (DeleteRequest):
    newOp->theStatus = GetValue;
    break;
  default:
    newOp->theStatus = SetValue;
  }
  const Uint32 tScanInfo = infoword & 0x3FFFF;
  const Uint32 tTakeOverFragment = infoword >> 20;
  {
    UintR scanInfo = 0;
    TcKeyReq::setTakeOverScanFlag(scanInfo, 1);
    TcKeyReq::setTakeOverScanFragment(scanInfo, tTakeOverFragment);
    TcKeyReq::setTakeOverScanInfo(scanInfo, tScanInfo);
    newOp->theScanInfo = scanInfo;
    newOp->theDistrKeyIndicator_ = 1;
    newOp->theDistributionKey = tTakeOverFragment;
  }
  
  // Copy the first 8 words of key info from KEYINF20 into TCKEYREQ
  TcKeyReq * tcKeyReq = CAST_PTR(TcKeyReq,newOp->theTCREQ->getDataPtrSend());
  Uint32 i = 0;
  for (i = 0; i < TcKeyReq::MaxKeyInfo && i < len; i++) {
    tcKeyReq->keyInfo[i] = * src++;
  }
  
  if(i < len){
    NdbApiSignal* tSignal = theNdb->getSignal();
    newOp->theTCREQ->next(tSignal); 
    
    Uint32 left = len - i;
    while(tSignal && left > KeyInfo::DataLength){
      tSignal->setSignal(GSN_KEYINFO);
      KeyInfo * keyInfo = CAST_PTR(KeyInfo, tSignal->getDataPtrSend());
      memcpy(keyInfo->keyData, src, 4 * KeyInfo::DataLength);
      src += KeyInfo::DataLength;
      left -= KeyInfo::DataLength;
      
      tSignal->next(theNdb->getSignal());
      tSignal = tSignal->next();
    }
    
    if(tSignal && left > 0){
      tSignal->setSignal(GSN_KEYINFO);
      KeyInfo * keyInfo = CAST_PTR(KeyInfo, tSignal->getDataPtrSend());
      memcpy(keyInfo->keyData, src, 4 * left);
    }      
  }
  /* create blob handles automatically for a delete - other ops must
   * create manually
   */
  if (opType == DeleteRequest && m_currentTable->m_noOfBlobs != 0) {
    for (unsigned i = 0; i < m_currentTable->m_columns.size(); i++) {
      NdbColumnImpl* c = m_currentTable->m_columns[i];
      assert(c != 0);
      if (c->getBlobType()) {
        if (newOp->getBlobHandle(pTrans, c) == NULL)
          return NULL;
      }
    }
  }
  
  return newOp;
}

NdbOperation*
NdbScanOperation::takeOverScanOpNdbRecord(OperationType opType,
                                          NdbTransaction* pTrans,
                                          const NdbRecord *record,
                                          char *row,
                                          const unsigned char *mask,
                                          const NdbOperation::OperationOptions *opts,
                                          Uint32 sizeOfOptions)
{
  int res;

  if (!m_attribute_record)
  {
    setErrorCodeAbort(4284);
    return NULL;
  }
  if (!record)
  {
    setErrorCodeAbort(4285);
    return NULL;
  }
  if (!m_keyInfo)
  {
    // Cannot take over lock if no keyinfo was requested
    setErrorCodeAbort(4604);
    return NULL;
  }
  if (record->flags & NdbRecord::RecIsIndex)
  {
    /* result_record must be a base table ndbrecord, not an index ndbrecord */
    setErrorCodeAbort(4340);
    return NULL;
  }

  NdbOperation *op= pTrans->getNdbOperation(record->table, NULL, true);
  if (!op)
    return NULL;

  pTrans->theSimpleState= 0;
  op->theStatus= NdbOperation::UseNdbRecord;
  op->theOperationType= opType;
  op->m_abortOption= AbortOnError;
  op->m_key_record= NULL;       // This means m_key_row has KEYINFO20 data
  op->m_attribute_record= record;
  /*
    The m_key_row pointer is only valid until next call of
    nextResult(fetchAllowed=true). But that is ok, since the lock is also
    only valid until that time, so the application must execute() the new
    operation before then.
   */

  /* Now find the current row, and extract keyinfo. */
  Uint32 idx= m_current_api_receiver;
  if (idx >= m_api_receivers_count)
    return NULL;
  const NdbReceiver *receiver= m_api_receivers[m_current_api_receiver];
  Uint32 infoword;
  res= receiver->get_keyinfo20(infoword, op->m_keyinfo_length, op->m_key_row);
  if (res==-1)
    return NULL;
  Uint32 scanInfo= 0;
  TcKeyReq::setTakeOverScanFlag(scanInfo, 1);
  Uint32 fragment= infoword >> 20;
  TcKeyReq::setTakeOverScanFragment(scanInfo, fragment);
  TcKeyReq::setTakeOverScanInfo(scanInfo, infoword & 0x3FFFF);
  op->theScanInfo= scanInfo;
  op->theDistrKeyIndicator_= 1;
  op->theDistributionKey= fragment;

  op->m_attribute_row= row;
  record->copyMask(op->m_read_mask, mask);

  if (opType == ReadRequest)
  {
    op->theLockMode= theLockMode;
    /*
     * Apart from taking over the row lock, we also support reading again,
     * though typical usage will probably use an empty mask to read nothing.
     */
    op->theReceiver.getValues(record, row);
  }
  else if (opType == DeleteRequest && row != NULL)
  {
    /* Delete with a 'pre-read' - prepare the Receiver */
    op->theReceiver.getValues(record, row);
  }


  /* Handle any OperationOptions */
  if (opts != NULL)
  {
    /* Delegate to static method in NdbOperation */
    Uint32 result = NdbOperation::handleOperationOptions (opType,
                                                          opts,
                                                          sizeOfOptions,
                                                          op);
    if (result != 0)
    {
      setErrorCodeAbort(result);
      return NULL;
    }
  }


  /* Setup Blob handles... */
  switch (opType)
  {
  case ReadRequest:
  case UpdateRequest:
    if (unlikely(record->flags & NdbRecord::RecHasBlob))
    {
      if (op->getBlobHandlesNdbRecord(pTrans) == -1)
        return NULL;
    }
    
    break;

  case DeleteRequest:
    /* Create blob handles if required, to properly delete all blob parts
     * If a pre-delete-read was requested, check that it does not ask for
     * Blob columns to be read.
     */
    if (unlikely(record->flags & NdbRecord::RecTableHasBlob))
    {
      if (op->getBlobHandlesNdbRecordDelete(pTrans,
                                            row != NULL) == -1)
        return NULL;
    }
    break;
  default:
    assert(false);
    return NULL;
  }

  /* Now prepare the signals to be sent...
   */
  int returnCode=op->buildSignalsNdbRecord(pTrans->theTCConPtr, 
                                           pTrans->theTransactionId);

  if (returnCode)
  {
    // buildSignalsNdbRecord should have set the error status
    // So we can return NULL
    return NULL;
  }

  return op;
}

NdbBlob*
NdbScanOperation::getBlobHandle(const char* anAttrName)
{
  /* We need the row KeyInfo for Blobs
   * Old Api scans have saved flags at this point
   */
  if (m_scanUsingOldApi)
    m_savedScanFlagsOldApi|= SF_KeyInfo;
  else
    m_keyInfo= 1;

  return NdbOperation::getBlobHandle(m_transConnection, 
                                     m_currentTable->getColumn(anAttrName));
}

NdbBlob*
NdbScanOperation::getBlobHandle(Uint32 anAttrId)
{
  /* We need the row KeyInfo for Blobs 
   * Old Api scans have saved flags at this point
   */
  if (m_scanUsingOldApi)
    m_savedScanFlagsOldApi|= SF_KeyInfo;
  else
    m_keyInfo= 1;

  return NdbOperation::getBlobHandle(m_transConnection, 
                                     m_currentTable->getColumn(anAttrId));
}

NdbRecAttr*
NdbScanOperation::getValue_NdbRecord_scan(const NdbColumnImpl* attrInfo,
                                          char* aValue)
{
  DBUG_ENTER("NdbScanOperation::getValue_NdbRecord_scan");
  int res;
  Uint32 ah;
  NdbRecAttr *ra;
  DBUG_PRINT("info", ("Column: %u", attrInfo->m_attrId));
  AttributeHeader::init(&ah, attrInfo->m_attrId, 0);
  res= insertATTRINFO(ah);
  if (res==-1)
    DBUG_RETURN(NULL);
  theInitialReadSize= theTotalCurrAI_Len - AttrInfo::SectionSizeInfoLength;
  ra= theReceiver.getValue(attrInfo, aValue);
  if (!ra)
  {
    setErrorCodeAbort(4000);
    DBUG_RETURN(NULL);
  }
  theErrorLine++;
  DBUG_RETURN(ra);
}

NdbRecAttr*
NdbScanOperation::getValue_NdbRecAttr_scan(const NdbColumnImpl* attrInfo,
                                           char* aValue)
{
  NdbRecAttr *recAttr= NULL;

  /* Get a RecAttr object, which is linked in to the Receiver's
   * RecAttr linked list, and return to caller
   */
  if (attrInfo != NULL) {
    m_no_disk_flag &= 
      (attrInfo->m_storageType == NDB_STORAGETYPE_MEMORY);
  
    recAttr = theReceiver.getValue(attrInfo, aValue);
    
    if (recAttr != NULL)
      theErrorLine++;
    else {
      /* MEMORY ALLOCATION ERROR */
      setErrorCodeAbort(4000);
    }
  }
  else {
    /* Attribute name or id not found in the table */
    setErrorCodeAbort(4004);
  }

  return recAttr;
}

NdbRecAttr*
NdbScanOperation::getValue_impl(const NdbColumnImpl *attrInfo, char *aValue)
{
  if (theStatus == UseNdbRecord)
    return getValue_NdbRecord_scan(attrInfo, aValue);
  else
    return getValue_NdbRecAttr_scan(attrInfo, aValue);
}

NdbIndexScanOperation::NdbIndexScanOperation(Ndb* aNdb)
  : NdbScanOperation(aNdb, NdbOperation::OrderedIndexScan)
{
  lowBound.keyRecAttr= highBound.keyRecAttr= NULL;
  initScanBoundStorageOldApi();
}

NdbIndexScanOperation::~NdbIndexScanOperation(){
}

/* This method initialises the old scan bound storage space.
 * It is called from the NdbIndexScanOperation constructor and
 * from NdbTransaction::scanIndex()
 */
void
NdbIndexScanOperation::initScanBoundStorageOldApi()
{
  assert(lowBound.keyRecAttr == NULL);
  assert(highBound.keyRecAttr == NULL);

  oldApiBoundDefined=false;
  lowBound.highestKey= 0;
  lowBound.highestSoFarIsStrict= false;
  /* Need to modify old Api scan bound handling code
   * if max attributes in key becomes > 32
   */
  assert(NDB_MAX_NO_OF_ATTRIBUTES_IN_KEY == 32);
  lowBound.keysPresentBitmap= 0;

  highBound= lowBound;
}

int
NdbIndexScanOperation::setBound(const char* anAttrName, int type, 
                                const void* aValue)
{
  return setBound(m_accessTable->getColumn(anAttrName), type, aValue);
}

int
NdbIndexScanOperation::setBound(Uint32 anAttrId, int type, 
                                const void* aValue)
{
  return setBound(m_accessTable->getColumn(anAttrId), type, aValue);
}

int
NdbIndexScanOperation::equal_impl(const NdbColumnImpl* anAttrObject, 
                                  const char* aValue)
{
  return setBound(anAttrObject, BoundEQ, aValue);
}

NdbRecAttr*
NdbIndexScanOperation::getValue_impl(const NdbColumnImpl* attrInfo, 
                                     char* aValue){
  /* Defer to ScanOperation implementation */
  // TODO : IndexScans always fetch PK columns via their key NdbRecord
  // If the user also requests them, we should avoid fetching them 
  // twice.
  return NdbScanOperation::getValue_impl(attrInfo, aValue);
}


/* Helper for setBound called via the old Api.  
 * Key bound information is stored in the operation for later
 * processing using the normal NdbRecord setBound interface.
 */
int
NdbIndexScanOperation::setBoundHelperOldApi(OldApiScanBoundInfo& boundInfo,
                                            Uint32 maxKeyRecordBytes,
                                            Uint32 index_attrId,
                                            Uint32 valueLen,
                                            bool inclusive,
                                            Uint32 byteOffset,
                                            Uint32 nullbit_byte_offset,
                                            Uint32 nullbit_bit_in_byte,
                                            const void *aValue)
{
  /* Grab RecAttr if necessary */
  if (boundInfo.keyRecAttr == NULL)
  {
    boundInfo.keyRecAttr= theNdb->getRecAttr();
    if (boundInfo.keyRecAttr != NULL)
    {
      boundInfo.keyRecAttr->setup(maxKeyRecordBytes, NULL);
    }
    else
    {
      /* Memory allocation error */
      setErrorCodeAbort(4000);
      return -1;
    }
  }
  
  Uint32 presentBitMask= (1 << (index_attrId & 0x1f));

  if ((boundInfo.keysPresentBitmap & presentBitMask) != 0)
  {
    /* setBound() called twice for same key */
    setErrorCodeAbort(4522);
    return -1;
  }

  /* Set bit in mask for key column presence */
  boundInfo.keysPresentBitmap |= presentBitMask;

  if ((index_attrId + 1) > boundInfo.highestKey)
  {
    // New highest key, check previous keys
    // are non-strict
    if (boundInfo.highestSoFarIsStrict)
    {
      /* Invalid set of range scan bounds */
      setErrorCodeAbort(4259);
      return -1;
    }
    boundInfo.highestKey= (index_attrId + 1);
    boundInfo.highestSoFarIsStrict= !inclusive;
  }
  else
  {
    /* Not highest, key, better not be strict */
    if (!inclusive)
    {
      /* Invalid set of range scan bounds */
      setErrorCodeAbort(4259);
      return -1;
    }
  }

  /* Copy data into correct part of RecAttr */
  assert(byteOffset + valueLen <= maxKeyRecordBytes);
  char *startOfRecord= boundInfo.keyRecAttr->aRef();

  memcpy(startOfRecord+byteOffset,
         aValue, 
         valueLen);

  /* Set Null bit */
  bool nullBit=(aValue == NULL);

  startOfRecord[nullbit_byte_offset]|= 
    (nullBit) << nullbit_bit_in_byte;

  return 0;
}

/*
 * Define bound on index column in range scan.
 */
int
NdbIndexScanOperation::setBound(const NdbColumnImpl* tAttrInfo, 
                                int type, const void* aValue)
{
  if (!tAttrInfo)
  {
    setErrorCodeAbort(4318);    // Invalid attribute
    return -1;
  }
  if (oldApiBoundDefined)
  {
    /* Only one scan bound allowed for non-NdbRecord setBound() API */
    setErrorCodeAbort(4513);
    return -1;
  }
  if (theOperationType == OpenRangeScanRequest &&
      (0 <= type && type <= 4)) 
  {
    const NdbRecord *key_record= m_accessTable->m_ndbrecord;
    const Uint32 maxKeyRecordBytes= key_record->m_row_size;

    Uint32 valueLen = 0;
    if (aValue != NULL)
      if (! tAttrInfo->get_var_length(aValue, valueLen)) {
        /* Length parameter in equal/setValue is incorrect */
        setErrorCodeAbort(4209);
        return -1;
      }
    
    /* Get details of column from NdbRecord */
    Uint32 byteOffset= 0;
    
    /* Get the Attr struct from the key NdbRecord for this index Attr */
    Uint32 attrId= tAttrInfo->m_attrId;

    if (attrId >= key_record->key_index_length)
    {
      /* Attempt to set bound on non key column */
      setErrorCodeAbort(4535);
      return -1;
    }
    Uint32 columnNum= key_record->key_indexes[ attrId ];

    if (columnNum >= key_record->noOfColumns)
    {
      /* Internal error in NdbApi */
      setErrorCodeAbort(4005);
      return -1;
    }

    NdbRecord::Attr attr= key_record->columns[ columnNum ];
    
    byteOffset= attr.offset;
    
    bool inclusive= ! ((type == BoundLT) || (type == BoundGT));

    /* Add to lower bound if required */
    if (type == BoundEQ ||
        type == BoundLE ||
        type == BoundLT )
    {
      if (setBoundHelperOldApi(lowBound,
                               maxKeyRecordBytes,
                               tAttrInfo->m_attrId,
                               valueLen,
                               inclusive,
                               byteOffset,
                               attr.nullbit_byte_offset,
                               attr.nullbit_bit_in_byte,
                               aValue) != 0)
        return -1;
    }

    /* Add to upper bound if required */
    if (type == BoundEQ ||
        type == BoundGE ||
        type == BoundGT)
    {
      if (setBoundHelperOldApi(highBound,
                               maxKeyRecordBytes,
                               tAttrInfo->m_attrId,
                               valueLen,
                               inclusive,
                               byteOffset,
                               attr.nullbit_byte_offset,
                               attr.nullbit_bit_in_byte,
                               aValue) != 0)             
        return -1;
    }
    return 0;
  } 
  else {
    /* Can only call setBound/equal() for an NdbIndexScanOperation */
    setErrorCodeAbort(4514);
    return -1;
  }
}


/* Method called just prior to scan execution to initialise
 * the passed in IndexBound for the scan using the information
 * stored by the old API's setBound() call.
 */
int
NdbIndexScanOperation::buildIndexBoundOldApi(IndexBound& ib)
{
  if (lowBound.highestKey != 0)
  {
    /* Have a low bound 
     * Check that a contiguous set of keys are supplied.
     * Setup low part of IndexBound
     */
    Uint32 expectedValue= (~(Uint32) 0) >> (32 - lowBound.highestKey);
    
    if (lowBound.keysPresentBitmap != expectedValue)
    {
      /* Invalid set of range scan bounds */
      setErrorCodeAbort(4259);
      return -1;
    }

    ib.low_key= lowBound.keyRecAttr->aRef();
    ib.low_key_count= lowBound.highestKey;
    ib.low_inclusive= !lowBound.highestSoFarIsStrict;
  }
  else
  {
    ib.low_key= NULL;
    ib.low_key_count= 0;
    ib.low_inclusive= false;
  }

  if (highBound.highestKey != 0)
  {
    /* Have a high bound 
     * Check that a contiguous set of keys are supplied.
     */
    Uint32 expectedValue= (~(Uint32) 0) >> (32 - highBound.highestKey);
    
    if (highBound.keysPresentBitmap != expectedValue)
    {
      /* Invalid set of range scan bounds */
      setErrorCodeAbort(4259);
      return -1;
    }

    ib.high_key= highBound.keyRecAttr->aRef();
    ib.high_key_count= highBound.highestKey;
    ib.high_inclusive= !highBound.highestSoFarIsStrict;
  }
  else
  {
    ib.high_key= NULL;
    ib.high_key_count= 0;
    ib.high_inclusive= false;
  }

  ib.range_no= 0;

  return 0;
}

/* Method called to release any resources allocated by the old 
 * Index Scan bound API
 */
void
NdbIndexScanOperation::releaseIndexBoundOldApi()
{
  if (lowBound.keyRecAttr != NULL)
  {
    theNdb->releaseRecAttr(lowBound.keyRecAttr);
    lowBound.keyRecAttr= NULL;
  }
  if (highBound.keyRecAttr != NULL)
  {
    theNdb->releaseRecAttr(highBound.keyRecAttr);
    highBound.keyRecAttr= NULL;
  }
  
  /* Re-initialise scan bound storage for the next
   * use of this NdbIndexScanOperation object
   */
  initScanBoundStorageOldApi();
}


/**
 * insertBOUNDS
 * Helper for ndbrecord_insert_bound, copying data into the
 * signal train and linking in new signals as required.
 */
int
NdbIndexScanOperation::insertBOUNDS(Uint32 * data, Uint32 sz){
  Uint32 len;
  Uint32 remaining = KeyInfo::DataLength - theTotalNrOfKeyWordInSignal;
  Uint32 * dst = theKEYINFOptr + theTotalNrOfKeyWordInSignal;
  do {
    len = (sz < remaining ? sz : remaining);
    memcpy(dst, data, 4 * len);
    
    if(sz >= remaining){
      /* Need to spill data into another signal */
      NdbApiSignal* tCurr = theLastKEYINFO;
      tCurr->setLength(KeyInfo::MaxSignalLength);
      NdbApiSignal* tSignal = tCurr->next();
      if(tSignal)
        ;
      else if((tSignal = theNdb->getSignal()) != 0)
      {
        /* Link new signal into train and set type */
        tCurr->next(tSignal);
        tSignal->setSignal(GSN_KEYINFO);
      } else {
        goto error;
      }
      theLastKEYINFO = tSignal;
      theKEYINFOptr = dst = ((KeyInfo*)tSignal->getDataPtrSend())->keyData;
      remaining = KeyInfo::DataLength;
      sz -= len;
      data += len;
    } else {
      len = (KeyInfo::DataLength - remaining) + len;
      break;
    }
  } while(true);   
  theTotalNrOfKeyWordInSignal = len;
  return 0;

error:
  setErrorCodeAbort(4228);    // XXX wrong code
  return -1;
}



int
NdbIndexScanOperation::ndbrecord_insert_bound(const NdbRecord *key_record,
                                              Uint32 column_index,
                                              const char *row,
                                              Uint32 bound_type)
{
  char buf[256];
  Uint32 currLen= theTotalNrOfKeyWordInSignal;
  Uint32 remaining= KeyInfo::DataLength - currLen;
  const NdbRecord::Attr *column= &key_record->columns[column_index];

  bool is_null= column->is_null(row);
  Uint32 len= 0;
  const void *aValue= row+column->offset;

  if (!is_null)
  {
    bool len_ok;
    /* Support for special mysqld varchar format in keys. */
    if (column->flags & NdbRecord::IsMysqldShrinkVarchar)
    {
      len_ok= column->shrink_varchar(row, len, buf);
      aValue= buf;
    }
    else
    {
      len_ok= column->get_var_length(row, len);
    }
    if (!len_ok) {
      setErrorCodeAbort(4209);
      return -1;
    }
  }

  /* Insert attribute header. */
  Uint32 tIndexAttrId= column->index_attrId;
  Uint32 sizeInWords= (len + 3) / 4;
  AttributeHeader ah(tIndexAttrId, sizeInWords << 2);
  const Uint32 ahValue= ah.m_value;
  const bool aligned= (UintPtr(aValue) & 3) == 0;

  /*
    The nobytes flag is false if there are extra padding bytes at the end,
    which we need to zero out.
  */
  const bool nobytes= (len & 0x3) == 0;
  const Uint32 totalLen= 2 + sizeInWords;
  Uint32 tupKeyLen= theTupKeyLen;
  if (remaining > totalLen && aligned && nobytes){
    Uint32 * dst= theKEYINFOptr + currLen;
    * dst ++ = bound_type;
    * dst ++ = ahValue;
    memcpy(dst, aValue, 4 * sizeInWords);
    theTotalNrOfKeyWordInSignal= currLen + totalLen;
  } else {
    if(!aligned || !nobytes){
      Uint32 tempData[2000];
      if (len > sizeof(tempData))
        len= sizeof(tempData);
      tempData[0] = bound_type;
      tempData[1] = ahValue;
      tempData[2 + (len >> 2)] = 0;
      memcpy(tempData+2, aValue, len);
      insertBOUNDS(tempData, 2+sizeInWords);
    } else {
      /* No alignment or zeroing required, just
       * need to spill into another signal */
      Uint32 buf[2] = { bound_type, ahValue };
      insertBOUNDS(buf, 2);
      insertBOUNDS((Uint32*)aValue, sizeInWords);
    }
  }
  theTupKeyLen= tupKeyLen + totalLen;

  return 0;
}

/* IndexScan readTuples - part of old scan API
 * This call does the minimum amount of validation and state
 * storage possible.  Most of the scan initialisation is done
 * later as part of processIndexScanDefs
 */
int
NdbIndexScanOperation::readTuples(LockMode lm,
                                  Uint32 scan_flags,
                                  Uint32 parallel,
                                  Uint32 batch)
{
  /* Defer to Scan Operation's readTuples */
  int res= NdbScanOperation::readTuples(lm, scan_flags, parallel, batch);
  
  /* Set up IndexScan specific members */
  if (res == 0 && 
      ( (int) m_accessTable->m_indexType ==
        (int) NdbDictionary::Index::OrderedIndex))
  {
    if (m_currentTable == m_accessTable){
      // Old way of scanning indexes, should not be allowed
      m_currentTable = theNdb->theDictionary->
        getTable(m_currentTable->m_primaryTable.c_str());
      assert(m_currentTable != NULL);
    }
    assert (m_currentTable != m_accessTable);
    // Modify operation state
    theStatus = GetValue;
    theOperationType  = OpenRangeScanRequest;
  }

  return res;
}

/* Most of the work of Index Scan definition for old and NdbRecord
 * Index scans is done in this method 
 */
int
NdbIndexScanOperation::processIndexScanDefs(LockMode lm,
                                            Uint32 scan_flags,
                                            Uint32 parallel,
                                            Uint32 batch)
{
  const bool order_by = scan_flags & SF_OrderBy;
  const bool order_desc = scan_flags & SF_Descending;
  const bool read_range_no = scan_flags & SF_ReadRangeNo;
  m_multi_range = scan_flags & SF_MultiRange;
  
  /* Defer to table scan method */
  int res = NdbScanOperation::processTableScanDefs(lm, 
                                                   scan_flags, 
                                                   parallel, 
                                                   batch);
  if(!res && read_range_no)
  {
    m_read_range_no = 1;
    Uint32 word = 0;
    AttributeHeader::init(&word, AttributeHeader::RANGE_NO, 0);
    if(insertATTRINFO(word) == -1)
      res = -1;
  }
  if (!res)
  {
    /**
     * Note that it is valid to have order_desc true and order_by false.
     *
     * This means that there will be no merge sort among partitions, but
     * each partition will still be returned in descending sort order.
     *
     * This is useful eg. if it is known that the scan spans only one
     * partition.
     */
     if (order_desc) {
       m_descending = true;
       ScanTabReq * req = CAST_PTR(ScanTabReq, theSCAN_TABREQ->getDataPtrSend());
       ScanTabReq::setDescendingFlag(req->requestInfo, true);
     }
     if (order_by) {
       m_ordered = true;
       Uint32 cnt = m_accessTable->getNoOfColumns() - 1;
       m_sort_columns = cnt; // -1 for NDB$NODE
       m_current_api_receiver = m_sent_receivers_count;
       m_api_receivers_count = m_sent_receivers_count;
     }
    
    /* Should always have NdbRecord at this point */
    assert (m_attribute_record);
  }
  m_this_bound_start = 0;
  m_first_bound_word = theKEYINFOptr;
  m_num_bounds = 0;
  m_previous_range_num = 0;

  return res;
}

int
NdbIndexScanOperation::compare_ndbrecord(const NdbReceiver *r1,
                                         const NdbReceiver *r2) const
{
  Uint32 i;
  int jdir= 1 - 2 * (int)m_descending;
  const NdbRecord *key_record= m_key_record;
  const NdbRecord *result_record= m_attribute_record;

  assert(jdir == 1 || jdir == -1);

  const char *a_row= r1->peek_row();
  const char *b_row= r2->peek_row();

  /* First compare range_no if needed. */
  if (m_read_range_no)
  {
    Uint32 a_range_no= uint4korr(a_row+result_record->m_row_size);
    Uint32 b_range_no= uint4korr(b_row+result_record->m_row_size);
   if (a_range_no != b_range_no)
      return (a_range_no < b_range_no ? -1 : 1);
  }

  for (i= 0; i<key_record->key_index_length; i++)
  {
    const NdbRecord::Attr *key_col =
      &key_record->columns[key_record->key_indexes[i]];
    assert(key_col->attrId < result_record->m_attrId_indexes_length);
    int col_idx = result_record->m_attrId_indexes[key_col->attrId];
    assert(col_idx >= 0);
    assert((Uint32)col_idx < result_record->noOfColumns);
    const NdbRecord::Attr *result_col = &result_record->columns[col_idx];

    bool a_is_null= result_col->is_null(a_row);
    bool b_is_null= result_col->is_null(b_row);
    if (a_is_null)
    {
      if (!b_is_null)
        return -1 * jdir;
    }
    else
    {
      if (b_is_null)
        return 1 * jdir;

      Uint32 offset= result_col->offset;
      Uint32 maxSize= result_col->maxSize;
      const char *a_ptr= a_row + offset;
      const char *b_ptr= b_row + offset;
      void *info= result_col->charset_info;
      int res=
        (*result_col->compare_function)
            (info, a_ptr, maxSize, b_ptr, maxSize, true);
      if (res)
      {
        assert(res != NdbSqlUtil::CmpUnknown);
        return res * jdir;
      }
    }
  }

  return 0;
}

/* This function performs the merge sort of the parallel ordered index scans
 * to produce a single sorted stream of rows to the application.
 *
 * To ensure the correct ordering, before a row can be returned, the function
 * must ensure that all fragments have either returned at least one row, or 
 * indicated that they have no more rows to return.
 *
 * The function maintains an array of receivers, one per fragment, sorted by
 * the relative ordering of their next rows.  Each time a row is taken from 
 * the 'top' receiver, it is re-inserted in the ordered list of receivers
 * which requires O(log2(NumReceivers)) comparisons.
 */
int
NdbIndexScanOperation::next_result_ordered_ndbrecord(const char * & out_row,
                                                     bool fetchAllowed,
                                                     bool forceSend)
{
  Uint32 current;

  /*
    Retrieve more rows if necessary, then sort the array of receivers.

    The special case m_current_api_receiver==theParallelism is for the
    initial call, where we need to wait for and sort all receviers.
  */
  if (m_current_api_receiver==theParallelism ||
      !m_api_receivers[m_current_api_receiver]->nextResult())
  {
    if (!fetchAllowed)
      return 2;                                 // No more data available now

    /* Wait for all receivers to be retrieved. */
    int count= ordered_send_scan_wait_for_all(forceSend);
    if (count == -1)
      return -1;

    /*
      Insert all newly retrieved receivers in sorted array.
      The receivers are left in m_conf_receivers for us to move into place.
    */
    current= m_current_api_receiver;
    for (int i= 0; i < count; i++)
      ordered_insert_receiver(current--, m_conf_receivers[i]);
    m_current_api_receiver= current;
  }
  else
  {
    /*
      Just make sure the first receiver (from which we just returned a row, so
      it may no longer be in the correct sort position) is placed correctly.
    */
    current= m_current_api_receiver;
    ordered_insert_receiver(current + 1, m_api_receivers[current]);
  }

  /* Now just return the next row (if any). */
  if (current < theParallelism && m_api_receivers[current]->nextResult())
  {
    out_row=  m_api_receivers[current]->get_row();
    return 0;
  }
  else
  {
    theError.code= -1;
    return 1;                                   // End-of-file
  }
}

/* Insert a newly fully-retrieved receiver in the correct sorted place. */
void
NdbIndexScanOperation::ordered_insert_receiver(Uint32 start,
                                               NdbReceiver *receiver)
{
  /*
    Binary search to find the position of the first receiver with no rows
    smaller than the first row for this receiver. We need to insert this
    receiver just before that position.
  */
  Uint32 first= start;
  Uint32 last= theParallelism;
  while (first < last)
  {
    Uint32 idx= (first+last)/2;
    int res= compare_ndbrecord(receiver, m_api_receivers[idx]);
    if (res <= 0)
      last= idx;
    else
      first= idx+1;
  }

  /* Move down any receivers that go before this one, then insert it. */
  if (last > start)
    memmove(&m_api_receivers[start-1],
            &m_api_receivers[start],
            (last - start) * sizeof(m_api_receivers[0]));
  m_api_receivers[last-1]= receiver;
}

/*
  This method is called during (NdbRecord) ordered index scans when all rows
  from one batch of one fragment scan are exhausted (identified by
  m_current_api_receiver).

  It sends a SCAN_NEXTREQ signal for the fragment and waits for the batch to
  be fully received.

  As a special case, it is also called at the start of the scan. In this case,
  no signal is sent, it just waits for the initial batch to be fully received
  from all fragments.

  The method returns -1 for error, and otherwise the number of fragments that
  were received (this will be 0 or 1, except for the initial call where it
  will be equal to theParallelism).

  The NdbReceiver object(s) are left in the m_conf_receivers array. Note that
  it is safe to read from m_conf_receivers without mutex protection immediately
  after return from this method; as all fragments are fully received no new
  receivers can enter that array until the next call to this method.
*/
int
NdbIndexScanOperation::ordered_send_scan_wait_for_all(bool forceSend)
{
  TransporterFacade* tp= theNdb->theImpl->m_transporter_facade;

  PollGuard poll_guard(tp, &theNdb->theImpl->theWaiter,
                       theNdb->theNdbBlockNumber);
  if(theError.code)
    return -1;

  Uint32 seq= theNdbCon->theNodeSequence;
  Uint32 nodeId= theNdbCon->theDBnode;
  Uint32 timeout= tp->m_waitfor_timeout;
  if (seq == tp->getNodeSequence(nodeId) &&
      !send_next_scan_ordered(m_current_api_receiver))
  {
    while (m_sent_receivers_count > 0 && !theError.code)
    {
      int ret_code= poll_guard.wait_scan(3*timeout, nodeId, forceSend);
      if (ret_code == 0 && seq == tp->getNodeSequence(nodeId))
        continue;
      if(ret_code == -1){
        setErrorCode(4008);
      } else {
        setErrorCode(4028);
      }
      return -1;
    }

    if(theError.code){
      setErrorCode(theError.code);
      return -1;
    }

    Uint32 new_receivers= m_conf_receivers_count;
    m_conf_receivers_count= 0;
    return new_receivers;
  } else {
    setErrorCode(4028);
    return -1;
  }
}

/*
  This method is used in ordered index scan to acknowledge the reception of
  one batch of fragment scan rows and request the sending of another batch (it
  sends a SCAN_NEXTREQ signal with one scan fragment record pointer).

  It is called with the argument IDX set to the value of
  m_current_api_receiver, the receiver for the fragment scan to acknowledge.
  This receiver is moved from the m_api_receivers array to the
  m_sent_receivers array.

  This method is called with the PollGuard mutex held on the transporter.
*/
int
NdbIndexScanOperation::send_next_scan_ordered(Uint32 idx)
{
  if(idx == theParallelism)
    return 0;
  
  NdbReceiver* tRec = m_api_receivers[idx];
  NdbApiSignal tSignal(theNdb->theMyRef);
  tSignal.setSignal(GSN_SCAN_NEXTREQ);
  
  Uint32 last = m_sent_receivers_count;
  Uint32* theData = tSignal.getDataPtrSend();
  Uint32* prep_array = theData + 4;
  
  m_current_api_receiver = idx + 1;
  if((prep_array[0] = tRec->m_tcPtrI) == RNIL)
  {
    if(DEBUG_NEXT_RESULT)
      ndbout_c("receiver completed, don't send");
    return 0;
  }
  
  theData[0] = theNdbCon->theTCConPtr;
  theData[1] = 0;
  Uint64 transId = theNdbCon->theTransactionId;
  theData[2] = transId;
  theData[3] = (Uint32) (transId >> 32);
  
  /**
   * Prepare ops
   */
  m_sent_receivers[last] = tRec;
  tRec->m_list_index = last;
  tRec->prepareSend();
  m_sent_receivers_count = last + 1;
  
  Uint32 nodeId = theNdbCon->theDBnode;
  TransporterFacade * tp = theNdb->theImpl->m_transporter_facade;
  tSignal.setLength(4+1);
  int ret= tp->sendSignal(&tSignal, nodeId);
  return ret;
}

int
NdbScanOperation::close_impl(TransporterFacade* tp, bool forceSend,
                             PollGuard *poll_guard)
{
  Uint32 seq = theNdbCon->theNodeSequence;
  Uint32 nodeId = theNdbCon->theDBnode;
  
  if(seq != tp->getNodeSequence(nodeId))
  {
    theNdbCon->theReleaseOnClose = true;
    return -1;
  }
  
  Uint32 timeout = tp->m_waitfor_timeout;
  /**
   * Wait for outstanding
   */
  while(theError.code == 0 && m_sent_receivers_count)
  {
    int return_code= poll_guard->wait_scan(3*timeout, nodeId, forceSend);
    switch(return_code){
    case 0:
      break;
    case -1:
      setErrorCode(4008);
    case -2:
      m_api_receivers_count = 0;
      m_conf_receivers_count = 0;
      m_sent_receivers_count = 0;
      theNdbCon->theReleaseOnClose = true;
      return -1;
    }
  }

  if(theError.code)
  {
    m_api_receivers_count = 0;
    m_current_api_receiver = m_ordered ? theParallelism : 0;
  }


  /**
   * move all conf'ed into api
   *   so that send_next_scan can check if they needs to be closed
   */
  Uint32 api = m_api_receivers_count;
  Uint32 conf = m_conf_receivers_count;

  if(m_ordered)
  {
    /**
     * Ordered scan, keep the m_api_receivers "to the right"
     */
    memmove(m_api_receivers, m_api_receivers+m_current_api_receiver, 
            (theParallelism - m_current_api_receiver) * sizeof(char*));
    api = (theParallelism - m_current_api_receiver);
    m_api_receivers_count = api;
  }
  
  if(DEBUG_NEXT_RESULT)
    ndbout_c("close_impl: [order api conf sent curr parr] %d %d %d %d %d %d",
             m_ordered, api, conf, 
             m_sent_receivers_count, m_current_api_receiver, theParallelism);
  
  if(api+conf)
  {
    /**
     * There's something to close
     *   setup m_api_receivers (for send_next_scan)
     */
    memcpy(m_api_receivers+api, m_conf_receivers, conf * sizeof(char*));
    m_api_receivers_count = api + conf;
    m_conf_receivers_count = 0;
  }
  
  // Send close scan
  if(send_next_scan(api+conf, true) == -1)
  {
    theNdbCon->theReleaseOnClose = true;
    return -1;
  }
  
  /**
   * wait for close scan conf
   */
  while(m_sent_receivers_count+m_api_receivers_count+m_conf_receivers_count)
  {
    int return_code= poll_guard->wait_scan(3*timeout, nodeId, forceSend);
    switch(return_code){
    case 0:
      break;
    case -1:
      setErrorCode(4008);
    case -2:
      m_api_receivers_count = 0;
      m_conf_receivers_count = 0;
      m_sent_receivers_count = 0;
      theNdbCon->theReleaseOnClose = true;
      return -1;
    }
  }

  /* Rather nasty way to clean up IndexScan resources if
   * any 
   */
  if (theOperationType == OpenRangeScanRequest)
  {
    NdbIndexScanOperation *isop= 
      reinterpret_cast<NdbIndexScanOperation*> (this);

    /* Release any Index Bound resources */
    isop->releaseIndexBoundOldApi();
  }

  /* Free any scan-owned ScanFilter generated InterpretedCode
   * object (old Api only)
   */
  freeInterpretedCodeOldApi();

  return 0;
}

void
NdbScanOperation::reset_receivers(Uint32 parallell, Uint32 ordered){
  for(Uint32 i = 0; i<parallell; i++){
    m_receivers[i]->m_list_index = i;
    m_prepared_receivers[i] = m_receivers[i]->getId();
    m_sent_receivers[i] = m_receivers[i];
    m_conf_receivers[i] = 0;
    m_api_receivers[i] = 0;
    m_receivers[i]->prepareSend();
  }
  
  m_api_receivers_count = 0;
  m_current_api_receiver = 0;
  m_sent_receivers_count = 0;
  m_conf_receivers_count = 0;
}

int
NdbIndexScanOperation::end_of_bound(Uint32 no)
{
  DBUG_ENTER("end_of_bound");
  DBUG_PRINT("info", ("Range number %u", no));

  /* Multirange no longer supported from old scan API */
  if (no > 0 || oldApiBoundDefined)
    DBUG_RETURN(-1);
  
  oldApiBoundDefined= true;
  DBUG_RETURN(0);
}

int
NdbIndexScanOperation::get_range_no()
{
  assert(m_attribute_record);

  if (m_read_range_no)
  {
    Uint32 idx= m_current_api_receiver;
    if (idx >= m_api_receivers_count)
      return -1;
    
    const NdbReceiver *tRec= m_api_receivers[m_current_api_receiver];
    return tRec->get_range_no();
  }
  return -1;
}
