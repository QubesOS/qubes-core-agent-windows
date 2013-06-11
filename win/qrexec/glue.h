/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */



int peer_server_init(int port);
int read_ready_vchan_ext();
int read_all_vchan_ext(void *buf, int size);
int write_all_vchan_ext(void *buf, int size);
int buffer_space_vchan_ext();

enum {
	WRITE_STDIN_OK = 0x200,
	WRITE_STDIN_BUFFERED,
	WRITE_STDIN_ERROR
};

int flush_client_data(int fd, int client_id, struct buffer *buffer);
int write_stdin(int fd, int client_id, char *data, int len,
		struct buffer *buffer);
void set_nonblock(int fd);
int fork_and_flush_stdin(int fd, struct buffer *buffer);
extern libvchan_t *ctrl;
