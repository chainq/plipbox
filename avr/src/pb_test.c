/*
 * pb_test.c: plipbox test mode
 *
 * Written by
 *  Christian Vogelgsang <chris@vogelgsang.org>
 *
 * This file is part of plipbox.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "pb_test.h"

#include "uartutil.h"
#include "uart.h"
#include "eth_state.h"
#include "pb_state.h"
#include "pb_io.h"
#include "pb_proto.h"
#include "param.h"
#include "timer.h"
#include "util.h"
#include "stats.h"
#include "dump.h"

#define TEST_STATE_OFF    0
#define TEST_STATE_ENTER  1
#define TEST_STATE_ACTIVE 2
#define TEST_STATE_LEAVE  3

static u32 trigger_ts;
static u16 count;
static u16 errors;

static u08 toggle_request;
static u08 auto_mode;
static u08 silent_mode;
static u08 state = TEST_STATE_OFF;

// ----- Helpers -----

static void dump_result(u08 is_tx, u32 delta, u16 rate)
{
  PGM_P str = is_tx ? PSTR("[TX] ") : PSTR("[RX] ");
  uart_send_time_stamp_spc();
  uart_send_pstring(str);

  // if everything is ok then print only rate
  if(errors == 0) {
    uart_send_rate_kbs(rate);
  }
  // errors occurred
  else {
    uart_send_pstring(PSTR("ERROR "));
    uart_send_hex_word(errors);
    uart_send_spc();
    uart_send_hex_word(count);
    uart_send_spc();
    uart_send_delta(delta);
  }
  if(is_tx) {
    uart_send_spc();
    uart_send_delta(trigger_ts);
  }
  uart_send_crlf();
}

// ----- RX Calls -----

static void rx_begin(u16 *pkt_size)
{
  errors = 0;
  count = 0;

  // check packet size
  if(*pkt_size != param.test_plen) {
    errors = 1;
    uart_send_pstring(PSTR("ERR: size\r\n"));
  }
}

static void rx_data(u08 *data)
{
  // check dst mac
  if(count < 6) {
    if(*data != 0xff) {
      errors++;
      uart_send_pstring(PSTR("ERR: dst mac\r\n"));
    }
  }
  // check src mac
  else if(count < 12) {
    u08 mac_byte = param.mac_addr[count-6];
    if(*data != mac_byte) {
      errors++;
      uart_send_pstring(PSTR("ERR: src mac\r\n"));
    }
  }
  // check type
  else if(count == 12) {
    u08 ptype_hi = (u08)(param.test_ptype >> 8);
    if(*data != ptype_hi) {
      errors++;
      uart_send_pstring(PSTR("ERR: type hi\r\n"));
    }
  }
  else if(count == 13) {
    u08 ptype_lo = (u08)(param.test_ptype & 0xff);
    if(*data != ptype_lo) {
      errors++;
      uart_send_pstring(PSTR("ERR: type lo\r\n"));
    }    
  }
  // data 
  else {
    u08 val = (u08)((count - 14) & 0xff);
    if(*data != val) {
      errors++;
      uart_send_pstring(PSTR("ERR: data\r\n"));
      uart_send_hex_word(count);
      uart_send_crlf();
    }
  }
  count++;
}

static void rx_end(u16 pkt_size)
{
  // record error if transfer was aborted
  if(pkt_size != param.test_plen) {
    errors += 1;
  }
}

// ----- TX Calls -----

static void tx_begin(u16 *pkt_size)
{
  // calc delta of tx trigger
  trigger_ts = time_stamp - trigger_ts;
  *pkt_size = param.test_plen;
 
  count = 0;
  errors = 0;
}

static void tx_data(u08 *data)
{
  // dst mac
  if(count < 6) {
    *data = 0xff; // broadcast
  }
  // src mac
  else if(count < 12) {
    u08 mac_byte = param.mac_addr[count-6];
    *data = mac_byte; // my mac
  }
  // type
  else if(count == 12) {
    u08 ptype_hi = (u08)(param.test_ptype >> 8);
    *data = ptype_hi;
  }
  else if(count == 13) {
    u08 ptype_lo = (u08)(param.test_ptype & 0xff);
    *data = ptype_lo;
  }
  // data
  else {
    *data = (u08)((count - 14) & 0xff);
  }
  count ++;
}

static void tx_end(u16 pkt_size)
{
  // record error if transfer was aborted
  if(pkt_size != param.test_plen) {
    errors += 1;
  }
}

// ----- function table -----

static pb_proto_funcs_t funcs = {
  .send_begin = rx_begin,
  .send_data = rx_data,
  .send_end = rx_end,
  
  .recv_begin = tx_begin,
  .recv_data = tx_data,
  .recv_end = tx_end
};


u08 pb_test_worker(void)
{
  // call protocol handler (low level transmit)
  u08 cmd;
  u16 size;
  u32 start = time_stamp;
  u08 status = pb_proto_handle(&cmd, &size);
  u32 delta = time_stamp - start;
  u16 rate = calc_rate_kbs(count, delta);
  u08 is_tx = (cmd == PBPROTO_CMD_SEND) || (cmd == PBPROTO_CMD_SEND_BURST);

  // nothing done... return
  if(status == PBPROTO_STATUS_IDLE) {
    return PB_TEST_IDLE; // inactive
  }

  // pb proto not ok -> error!
  if(status != PBPROTO_STATUS_OK) {
    errors++;
  }

  // no errors?
  if(errors == 0) {
    // account data
    if(is_tx) {
      stats.tx_cnt++;
      stats.tx_bytes+=count;
      if(stats.tx_max_rate < rate) {
        stats.tx_max_rate = rate;
      }
    } else {
      stats.rx_cnt++;
      stats.rx_bytes+=count;
      if(stats.rx_max_rate < rate) {
        stats.rx_max_rate = rate;
      }
    }

    // dump result?
    if(!silent_mode) {
      // in interactive mode show result
      dump_result(is_tx, delta, rate);
    }

    // next iteration?
    if(is_tx) {
      if(auto_mode) {
        // next iteration after 
        pb_test_send_packet(1);
      } else {
        silent_mode = 0;
      }
    }

    return PB_TEST_OK;
  }
  // pb proto failed with an error
  else {
    // dump error
    dump_pb_cmd(cmd, status, size, delta, 0);

    dump_result(is_tx, delta, rate);
    // account data
    if(is_tx) {
      stats.tx_err++;
    } else {
      stats.rx_err++;
    }

    // disable auto mode
    if(auto_mode) {
      pb_test_toggle_auto();
    }

    return PB_TEST_ERROR;
  }
}

void pb_test_send_packet(u08 silent)
{
  if(state != TEST_STATE_ACTIVE) {
    return;
  }

  silent_mode = silent;
  trigger_ts = time_stamp;
  pb_proto_request_recv();
}

void pb_test_toggle_auto(void)
{
  if(state != TEST_STATE_ACTIVE) {
    return;
  }

  auto_mode = !auto_mode;

  uart_send_time_stamp_spc();
  uart_send_pstring(PSTR("[AUTO] "));
  if(auto_mode) {
    uart_send_pstring(PSTR("on"));
  } else {
    uart_send_pstring(PSTR("off"));
  }
  uart_send_crlf();

  if(auto_mode) {
    // send first packet
    pb_test_send_packet(1);
    // clear stats
    stats_reset();
  }
}

// ----- Test Mode Handling -----

void pb_test_toggle_mode(void)
{
  toggle_request = 1;
}

static void dump_state(void)
{
  uart_send_time_stamp_spc();
  uart_send_pstring(PSTR("[TEST] "));
  
  PGM_P str = 0;
  switch(state) {
    case TEST_STATE_OFF:
      str = PSTR("off");
      break;
    case TEST_STATE_ENTER:
      str = PSTR("enter");
      break;
    case TEST_STATE_ACTIVE:
      str = PSTR("active");
      break;
    case TEST_STATE_LEAVE:
      str = PSTR("leave");
      break;
  }

  uart_send_pstring(str);
  uart_send_crlf();
}

u08 pb_test_state(u08 eth_state, u08 pb_state)
{
  switch(state) {
    case TEST_STATE_OFF:
      if(toggle_request) {
        // disable ethernet
        eth_state_shutdown();

        state = TEST_STATE_ENTER;
        dump_state();
        toggle_request = 0;
      }
      break;
    case TEST_STATE_ENTER:
      // wait for ethernet off, but required pb link
      if(eth_state == ETH_STATE_OFF) {
        // setup handlers for pb testing
        pb_proto_init(&funcs);

        state = TEST_STATE_ACTIVE;
        dump_state();
      }
      // skip to active if abort was toggled
      if(toggle_request) {
        state = TEST_STATE_ACTIVE;
      }
      break;
    case TEST_STATE_ACTIVE:
      if(toggle_request) {
        // enable ethernet
        eth_state_init();
        // restore plibbox io handler
        pb_io_init();

        state = TEST_STATE_LEAVE;
        dump_state();
        toggle_request = 0;
      }
      break;
    case TEST_STATE_LEAVE:
      // wait for ethernet on again
      if((eth_state == ETH_STATE_LINK_DOWN)||(eth_state == ETH_STATE_LINK_UP)) {
        state = TEST_STATE_OFF;
        dump_state();
      }
  }
  return state == TEST_STATE_ACTIVE;
}