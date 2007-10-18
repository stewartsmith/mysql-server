/* Copyright (C) 2006 MySQL AB

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

// MemoryResultSetColumn.h: interface for the MemoryResultSetColumn class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_MEMORYRESULTSETCOLUMN_H__E4CF0404_0ECC_11D3_AB71_0000C01D2301__INCLUDED_)
#define AFX_MEMORYRESULTSETCOLUMN_H__E4CF0404_0ECC_11D3_AB71_0000C01D2301__INCLUDED_

#if _MSC_VER >= 1000
#pragma once
#endif // _MSC_VER >= 1000

class MemoryResultSetColumn  
{
public:
	MemoryResultSetColumn(const char *name, const char *label, int type, int displaySize, int precision, int scale);
	virtual ~MemoryResultSetColumn();

	JString		name;
	JString		label;
	int			displaySize;
	int			precision;
	int			scale;
	int			type;
};

#endif // !defined(AFX_MEMORYRESULTSETCOLUMN_H__E4CF0404_0ECC_11D3_AB71_0000C01D2301__INCLUDED_)
