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

// CoterieRange.cpp: implementation of the CoterieRange class.
//
//////////////////////////////////////////////////////////////////////

#include "Engine.h"
#include "CoterieRange.h"
#include "Coterie.h"
#include "Socket.h"

#ifdef _DEBUG
#undef THIS_FILE
static const char THIS_FILE[]=__FILE__;
#endif

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CoterieRange::CoterieRange(const char *from)
{
	fromString = from;
	fromIPaddress = toIPaddress = Socket::translateAddress (fromString);
}

CoterieRange::CoterieRange(const char *from, const char *to)
{
	fromString = from;
	fromIPaddress = Socket::translateAddress (fromString);
	toString = to;
	toIPaddress = Socket::translateAddress (toString);
}

CoterieRange::~CoterieRange()
{
}


bool CoterieRange::validateAddress(int32 address)
{
	return address >= fromIPaddress && address <= toIPaddress;
}
