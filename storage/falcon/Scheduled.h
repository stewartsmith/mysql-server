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

// Scheduled.h: interface for the Scheduled class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_SCHEDULED_H__9E13C6D3_1F3E_11D3_AB74_0000C01D2301__INCLUDED_)
#define AFX_SCHEDULED_H__9E13C6D3_1F3E_11D3_AB74_0000C01D2301__INCLUDED_

#if _MSC_VER >= 1000
#pragma once
#endif // _MSC_VER >= 1000

class Scheduled  
{
public:
	void scheduled (void *arg);
	Scheduled();
	virtual ~Scheduled();

};

#endif // !defined(AFX_SCHEDULED_H__9E13C6D3_1F3E_11D3_AB74_0000C01D2301__INCLUDED_)
