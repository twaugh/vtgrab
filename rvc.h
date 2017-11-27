/*
 * vtgrab - grab the foreground console for display on another machine
 * Copyright (C) 2000  Tim Waugh <twaugh@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * This file contains definitions needed for RVC communications.
 */

#define RVC_PROTOCOL_VERSION "RVC 001.000\n"

enum authentication_schemes {
	AuthFailed = 0,
	AuthNoAuth = 1,
	AuthVNC = 2
};

enum features {
	Feature_Key = 0,
	Feature_Pointer = 1,
	Feature_IncRectangle = 2,
	Feature_IncScroll = 3,
	Feature_IncWrite = 4,
	Feature_Crop = 5,
	Feature_Switch = 6,
	Feature_DisplayLock = 7,
	Feature_InputLock = 8,
	Feature_Shareable = 9,
	Feature_VNCIntegration = 10,
	Feature_SwitchRequest = 11,
	Feature_Push = 12,
};

enum server_messages {
	Msg_ServerInit = 0,
	Msg_IncrementalUpdate = 1,
	Msg_Switch = 2,
	Msg_Terminate = 128,
};

enum client_messages {
	Msg_ClientInit = 255,
	Msg_FullUpdateRequest = 254,
	Msg_Key = 253,
	Msg_Pointer = 252,
	Msg_SwitchRequest = 251,
	Msg_PushStart = 250,
	Msg_PushStop = 249,
};

enum updatetypes {
	UpdateType_Rectangle = 2
};

struct ClientInitialisation_fixedpart 
{
	uint8_t message_type;
	uint8_t pad1;
	uint8_t pad2;
	uint8_t pad3;
	uint32_t updatems;
	uint8_t rows;
	uint8_t cols;
	uint8_t pad4;
	uint8_t num_features;
};
