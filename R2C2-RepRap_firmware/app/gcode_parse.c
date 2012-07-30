/* Copyright (C) 2009-2010 Michael Moon aka Triffid_Hunter   */
/* Copyright (c) 2011 Jorge Pinto - casainho@gmail.com       */
/* All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.
   * Neither the name of the copyright holders nor the names of
     contributors may be used to endorse or promote products derived
     from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
*/


#include  <string.h>
#include  <stdbool.h>
#include  <ctype.h>

#include  "app_config.h"
#include	"gcode_parse.h"
#include	"gcode_process.h"
#include	"lw_io.h"
#include  "machine.h"


#define crc(a, b)		(a ^ b)

/*
	Internal representations of values uses double for motion axes, feedrate.
*/

// This holds the parsed command used by gcode_process
GCODE_COMMAND next_target;

// context variables used during parsing of a line
static char last_field = 0; // letter code

static struct {
  uint8_t   sign; 
  int       exponent;
  } read_digit;

static double value;


// accept the next character and process it
static void gcode_parse_char(tLineBuffer *pLine, uint8_t c);

// uses the global variable next_target.N
static void request_resend(tGcodeInputMsg *pGcodeInputMsg);


void gcode_parse_init(void)
{
#if 0
  steps_per_m_x = ((uint32_t) (config.steps_per_mm_x * 1000.0));
  steps_per_m_y = ((uint32_t) (config.steps_per_mm_y * 1000.0));
  steps_per_m_z = ((uint32_t) (config.steps_per_mm_z * 1000.0));
  steps_per_m_e = ((uint32_t) (config.steps_per_mm_e * 1000.0));

  // same as above with 25.4 scale factor
  steps_per_in_x = ((double) (25.4 * config.steps_per_mm_x));
  steps_per_in_y = ((double) (25.4 * config.steps_per_mm_y));
  steps_per_in_z = ((double) (25.4 * config.steps_per_mm_z));
  steps_per_in_e = ((double) (25.4 * config.steps_per_mm_e));
#endif  
  next_target.target.feed_rate = config.axis[Z_AXIS].homing_feedrate;
}


/*
	utility functions
*/


double power (double x, int exp)
{  
  double result = 1.0;
  while (exp--)  
    result = result * x;
  return result;
}

double inch_to_mm (double inches)
{  
  return inches * 25.4;
}

static uint8_t char_to_hex (char c)
{
  c= tolower(c);
  if ( (c >='0') && ( c <= '9'))
    return c - '0';
  else if ( (c >='a') && ( c <= 'f'))
    return c - 'a' + 10;
  else
    return 0;
}

static int hex_to_bin (char *s, int len)
{
  int ch_pos;
  int byte_pos;
  uint8_t byte_val;

  byte_pos = 0;
  ch_pos = 0;

  while (ch_pos < len)
  {
    byte_val = char_to_hex (s[ch_pos]);
    ch_pos ++;
    if (ch_pos < len)
    {
      byte_val = (byte_val << 4) + char_to_hex (s[ch_pos]);
    }
    ch_pos ++;

    s[byte_pos++] = byte_val;
  }
  return byte_pos;
}

/*
	public functions
*/


eParseResult gcode_parse_line (tGcodeInputMsg *pGcodeInputMsg) 
{
  //int j;
  tLineBuffer *pLine;
  eParseResult result = PR_OK;
  int len;

  pLine = pGcodeInputMsg->pLineBuf;

  //TODO: parse_char never detects errors? there must be some surely
  pLine->ch_pos = 0;
  while (pLine->ch_pos < pLine->len)
  {
    gcode_parse_char (pLine, pLine->data [pLine->ch_pos]);
    pLine->ch_pos++;
  }
    
  //TODO: more than one command per line
  //TODO: gcode context for each interface

	// end of line
	//if ((c == 10) || (c == 13)) 
	{
		if (
		#ifdef	REQUIRE_LINENUMBER
			(next_target.N >= next_target.N_expected) && (next_target.seen_N == 1)
		#else
			1
		#endif
			) {
			if (
				#ifdef	REQUIRE_CHECKSUM
				((next_target.checksum_calculated == next_target.checksum_read) && (next_target.seen_checksum == 1))
				#else
				((next_target.checksum_calculated == next_target.checksum_read) || (next_target.seen_checksum == 0))
				#endif
				) {
            // --------------------------------------------------------------------------
            //     SD
            // --------------------------------------------------------------------------
            if (sd_writing_file)
            {
              if (next_target.seen_M && (next_target.M >= 20) && (next_target.M <= 29) )
              {
                if (next_target.seen_M && (next_target.M == 29))
                { 
                  // M29 - stop writing
                  sd_writing_file = false;
                  sd_close (&file);
                  lw_fputs("Done saving file\r\n", pGcodeInputMsg->out_file);
                }
                else
                {
                  // else - do not write SD M-codes to file
                  lw_fputs("ok\r\n", pGcodeInputMsg->out_file);
                }
              }
              else
              {
                // lines in files must be LF terminated for sd_read_file to work
                if (pLine->data [pLine->len-1] == 13)
                  pLine->data [pLine->len-1] = 10;
                  
                if (next_target.seen_M && (next_target.M == 30))
                {
                  if (file_mode == FM_HEX_BIN)
                  {
                    len = hex_to_bin (next_target.str_param, strlen(next_target.str_param));
                  }
                  else
                  {
                    strcat (next_target.str_param, "\n");
                    len = strlen(next_target.str_param);
                  }

                  if (sd_write_to_file(next_target.str_param, len))
                    lw_fputs("ok\r\n", pGcodeInputMsg->out_file);
                  else
                    lw_fputs("error writing to file\r\n", pGcodeInputMsg->out_file);

                }
                else
                {
                  if (sd_write_to_file(pLine->data, pLine->len))
                    lw_fputs("ok\r\n", pGcodeInputMsg->out_file);
                  else
                    lw_fputs("error writing to file\r\n", pGcodeInputMsg->out_file);
                }
              }
            }
            // --------------------------------------------------------------------------
            //     Not in SD write mode
            // --------------------------------------------------------------------------
            else
            {
              // process
              result = process_gcode_command(pGcodeInputMsg);

              // expect next line number
              if (next_target.seen_N == 1)
                  next_target.N_expected = next_target.N + 1;
			      }
			}
			else 
      {
				lw_fprintf(pGcodeInputMsg->out_file, "Expected checksum %u\r\n", next_target.checksum_calculated);
				request_resend(pGcodeInputMsg);
        result = PR_RESEND;
			}
		}
		else 
    {
			lw_fprintf(pGcodeInputMsg->out_file, "Expected line number %ul\r\n", next_target.N_expected);
			request_resend(pGcodeInputMsg);
      result = PR_RESEND;
		}

		// reset variables
		next_target.seen_X = next_target.seen_Y = next_target.seen_Z = \
      next_target.seen_E = next_target.seen_F = next_target.seen_S = \
      next_target.seen_P = next_target.seen_N = next_target.seen_M = 0;

    next_target.seen_checksum = next_target.seen_semi_comment = \
                next_target.seen_parens_comment = next_target.checksum_read = \
                next_target.checksum_calculated = 0;

		next_target.str_pos = 0;

		// dont assume a G1 by default
		next_target.seen_G = 0;
		next_target.G = 0;

  //TODO:
		if (next_target.option_relative) 
    {
			next_target.target.x = next_target.target.y = next_target.target.z = 0.0;
			next_target.target.e = 0.0;
		}

    //
  	last_field = 0;
		read_digit.sign = read_digit.exponent = 0;
		value = 0;
	}


	return result;
    
}

/****************************************************************************
*                                                                           *
* Character Received - add it to our command                                *
*                                                                           *
****************************************************************************/

void gcode_parse_char(tLineBuffer *pLine, uint8_t c) 
{
  uint8_t save_ch;

	#ifdef ASTERISK_IN_CHECKSUM_INCLUDED
	if (next_target.seen_checksum == 0)
		next_target.checksum_calculated = crc(next_target.checksum_calculated, c);
	#endif

  save_ch = c;
	// uppercase
	if (c >= 'a' && c <= 'z')
		c &= ~32;

	// process previous field
	if (last_field) {
		// check if we're seeing a new field or end of line
		// any character will start a new field, even invalid/unknown ones
		if ((c >= 'A' && c <= 'Z') || c == '*' || (c == 10) || (c == 13)) {
		
		  // before using value, apply the sign
		  if (read_digit.sign)
		    value = -value;
		    
			switch (last_field) {
				case 'G':
					next_target.G = value;
					break;
				case 'M':
					next_target.M = value;
					// this is a bit hacky since string parameters don't fit in general G code syntax
					// NB: filename MUST start with a letter and MUST NOT contain spaces
					// letters will also be converted to uppercase
					if ( (next_target.M == 23) || (next_target.M == 28) || (next_target.M == 302) )
					{
					    next_target.getting_string = 1;
					}
					break;
				case 'X':
					if (next_target.option_inches)
						next_target.target.x = inch_to_mm(value);
					else
						next_target.target.x = value;
					break;
				case 'Y':
					if (next_target.option_inches)
						next_target.target.y = inch_to_mm(value);
					else
						next_target.target.y = value;
					break;
				case 'Z':
					if (next_target.option_inches)
						next_target.target.z = inch_to_mm(value);
					else
						next_target.target.z = value;
					break;
				case 'E':
					if (next_target.option_inches)
						next_target.target.e = inch_to_mm(value);
					else
						next_target.target.e = value;
					break;
				case 'F':
					if (next_target.option_inches)
						next_target.target.feed_rate = inch_to_mm(value);
					else
						next_target.target.feed_rate = value;
					break;
				case 'S':
						next_target.S = value;
					break;
				case 'P':
					// if this is dwell, multiply by 1000 to convert seconds to milliseconds
					if (next_target.G == 4)
						next_target.P = value * 1000.0;
					else
						next_target.P = value;
					break;
				case 'N':
					next_target.N = value;
					break;
				case '*':
					next_target.checksum_read = value;
					break;
			}
			// reset for next field
			last_field = 0;
			read_digit.sign = read_digit.exponent = 0;
			value = 0;
		}
	}

  if (next_target.getting_string)
  {
    if ((c == 10) || (c == 13) || ( c == ' ')  || ( c == '*') || ( c == '\"'))
      next_target.getting_string = 0;
    else
    {
      if (next_target.str_pos < sizeof(next_target.str_param))
      {
        next_target.str_param [next_target.str_pos++] = c;
        next_target.str_param [next_target.str_pos] = 0;
      }
    }      
  }

	// skip comments, filenames
	if (next_target.seen_semi_comment == 0 && next_target.seen_parens_comment == 0 && next_target.getting_string == 0) {
		// new field?
		if ((c >= 'A' && c <= 'Z') || c == '*') {
			last_field = c;
		}

		// process character
		switch (c) {
			// each currently known command is either G or M, so preserve previous G/M unless a new one has appeared
			// FIXME: same for T command
			case 'G':
				next_target.seen_G = 1;
				next_target.seen_M = 0;
				next_target.M = 0;
				break;
			case 'M':
				next_target.seen_M = 1;
				next_target.seen_G = 0;
				next_target.G = 0;
				break;
			case 'X':
				next_target.seen_X = 1;
				break;
			case 'Y':
				next_target.seen_Y = 1;
				break;
			case 'Z':
				next_target.seen_Z = 1;
				break;
			case 'E':
				next_target.seen_E = 1;
				break;
			case 'F':
				next_target.seen_F = 1;
				break;
			case 'S':
				next_target.seen_S = 1;
				break;
			case 'P':
				next_target.seen_P = 1;
				break;
			case 'N':
				next_target.seen_N = 1;
				break;
			case '*':
				next_target.seen_checksum = 1;
				break;

			// comments
			case ';':
			case '^':
				next_target.seen_semi_comment = 1;
				break;
			case '(':
				next_target.seen_parens_comment = 1;

        if ((pLine->ch_pos < pLine->len-4) && (strncmp (&pLine->data[pLine->ch_pos+1], "STR\"", 4) == 0))
        {
          next_target.str_pos = 0;
          next_target.getting_string = 1;
          pLine->ch_pos += 4;
        }
				break;

			// now for some numeracy
			case '-':
				read_digit.sign = 1;
				// force sign to be at start of number, so 1-2 = -2 instead of -12
				read_digit.exponent = 0;
				break;
			case '.':
				if (read_digit.exponent == 0)
					read_digit.exponent = 1;
				break;


			default:
				// can't do ranges in switch..case, so process actual digits here
				if (c >= '0' && c <= '9') {
					if (read_digit.exponent == 0)
					{
					  value = value * 10 + (c - '0');
					}
					else
					  value += (double)(c - '0') / power(10, read_digit.exponent);

					if (read_digit.exponent)
						read_digit.exponent++;
				}
		}
	} else if ( next_target.seen_parens_comment == 1)
  {
    if (c == ')')
      next_target.seen_parens_comment = 0; // recognize stuff after a (comment)
    else if (next_target.getting_string)
    {
      if (c == '\"')
        next_target.getting_string = 0;
    }
  }

	#ifndef ASTERISK_IN_CHECKSUM_INCLUDED
	if (next_target.seen_checksum == 0)
		next_target.checksum_calculated = crc(next_target.checksum_calculated, save_ch);
	#endif

	
}

/****************************************************************************
*                                                                           *
* Request a resend of the current line - used from various places.          *
*                                                                           *
* Relies on the global variable next_target.N being valid.                  *
*                                                                           *
****************************************************************************/

static void request_resend (tGcodeInputMsg *pGcodeInputMsg) 
{
	lw_fprintf(pGcodeInputMsg->out_file, "rs %ul\r\n", next_target.N);
}