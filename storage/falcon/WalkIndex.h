/* Copyright (C) 2008 MySQL AB

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

#ifndef _WALK_INDEX_H_
#define _WALK_INDEX_H_

#include "IndexWalker.h"
#include "IndexKey.h"
#include "IndexNode.h"

class Record;
class Btn;

class WalkIndex : public IndexWalker
{
public:
	WalkIndex(Index *index, Transaction *transaction, int flags, IndexKey *lower, IndexKey *upper);
	virtual ~WalkIndex(void);
	
	void			setNodes(int32 nextPage, int length, Btn* stuff);
	virtual Record* getNext(bool lockForUpdate);
	
	IndexKey	lowerBound;
	IndexKey	upperBound;
	Record		*record;
	UCHAR		*nodes;
	IndexNode	node;
	Btn			*endNodes;
	int32		nextPage;
};

#endif
