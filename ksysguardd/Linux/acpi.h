/*
    KSysGuard, the KDE System Guard

    Copyright (c) 2003 Stephan Uhlmann <su@su2.info>

    This program is free software; you can redistribute it and/or
    modify it under the terms of version 2 of the GNU General Public
    License as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifndef KSG_ACPI_H
#define KSG_ACPI_H

void initAcpi( struct SensorModul* );
void exitAcpi( void );

int updateAcpi( void );

void printAcpiBatFill( const char* );
void printAcpiBatFillInfo( const char* );
void printAcpiBatUsage( const char* );
void printAcpiBatUsageInfo( const char* );

#endif
