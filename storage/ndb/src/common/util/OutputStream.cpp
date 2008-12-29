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

#include <OutputStream.hpp>
#include <socket_io.h>

FileOutputStream::FileOutputStream(FILE * file){
  f = file;
}

int
FileOutputStream::print(const char * fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  const int ret = vfprintf(f, fmt, ap);
  va_end(ap);
  return ret;
}

int
FileOutputStream::println(const char * fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  const int ret = vfprintf(f, fmt, ap);
  va_end(ap);
  return ret + fprintf(f, "\n");
}

SocketOutputStream::SocketOutputStream(NDB_SOCKET_TYPE socket,
				       unsigned write_timeout_ms) :
  m_socket(socket),
  m_timeout_ms(write_timeout_ms),
  m_timedout(false),
  m_timeout_remain(write_timeout_ms)
{
}

int
SocketOutputStream::print(const char * fmt, ...){
  va_list ap;

  if(timedout())
    return -1;

  int time= 0;
  va_start(ap, fmt);
  int ret = vprint_socket(m_socket, m_timeout_ms, &time, fmt, ap);
  va_end(ap);

  if(ret >= 0)
    m_timeout_remain-=time;
  if((ret < 0 && errno==ETIMEDOUT) || m_timeout_remain<=0)
  {
    m_timedout= true;
    ret= -1;
  }

  return ret;
}
int
SocketOutputStream::println(const char * fmt, ...){
  va_list ap;

  if(timedout())
    return -1;

  int time= 0;
  va_start(ap, fmt);
  int ret = vprintln_socket(m_socket, m_timeout_ms, &time, fmt, ap);
  va_end(ap);

  if(ret >= 0)
    m_timeout_remain-=time;
  if ((ret < 0 && errno==ETIMEDOUT) || m_timeout_remain<=0)
  {
    m_timedout= true;
    ret= -1;
  }

  return ret;
}

#include <UtilBuffer.hpp>
#include <BaseString.hpp>

BufferedSockOutputStream::BufferedSockOutputStream(NDB_SOCKET_TYPE socket,
                                                   unsigned write_timeout_ms) :
  SocketOutputStream(socket, write_timeout_ms),
  m_buffer(*new UtilBuffer)
{
}

BufferedSockOutputStream::~BufferedSockOutputStream()
{
  delete &m_buffer;
}

int
BufferedSockOutputStream::print(const char * fmt, ...){
  char buf[1];
  va_list ap;
  int len;
  char* pos;

  // Find out length of string
  va_start(ap, fmt);
  len = BaseString::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // Grow buffer so it can hold the string
  if ((pos= (char*)m_buffer.append(len+1)) == 0)
    return -1;

  // Print string to buffer
  va_start(ap, fmt);
  len = BaseString::vsnprintf((char*)pos, len+1, fmt, ap);
  va_end(ap);

  return 0;
}

int
BufferedSockOutputStream::println(const char * fmt, ...){
  char buf[1];
  va_list ap;
  int len;
  char* pos;

  // Find out length of string
  va_start(ap, fmt);
  len = BaseString::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // Grow buffer so it can hold the string and the new line
  if ((pos= (char*)m_buffer.append(len+1)) == 0)
    return -1;

  // Print string to buffer
  va_start(ap, fmt);
  len = BaseString::vsnprintf((char*)pos, len+1, fmt, ap);
  va_end(ap);

  // Add newline
  pos+= len;
  *pos= '\n';

  return 0;
}

void BufferedSockOutputStream::flush(){
  int elapsed;
  if (write_socket(m_socket, m_timeout_ms, &elapsed,
                   (const char*)m_buffer.get_data(), m_buffer.length()) != 0)
  {
    fprintf(stderr, "Failed to flush buffer to socket, errno: %d\n", errno);
  }

  m_buffer.clear();
}

