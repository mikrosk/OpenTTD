/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file mint_m.cpp Music playback through the MIDI port of Atari computers running FreeMiNT. */

#include "../stdafx.h"
#include "../debug.h"
#include "../core/math_func.hpp"
#include "../base_media_base.h"
#include "midifile.hpp"
#include "midi.h"
#include "mint_m.h"
#include <algorithm>

#include <umidi.h>

#include "../safeguards.h"

/*
 * The songs are played by uMIDI, a MIDI library for Atari computers. Its
 * interrupt handlers run in supervisor mode while any process may be current,
 * so the memory of this program must be accessible in supervisor mode from any
 * context (linked with --msuper-memory). They only touch _midi and the song
 * set up by PlaySong().
 */

static const byte DEFAULT_CHANNEL_VOLUME  = 100;   ///< channel volume after a General MIDI reset
static const uint32 RESET_STEP_TIME       = 50000; ///< microseconds between the steps of the device reset before each song
static const uint32 MICROSECONDS_PER_BYTE = 320;   ///< transmission time of one byte at 31250 baud with start and stop bits
static const uint32 VOLUME_INTERVAL       = 42;    ///< milliseconds between two music volume changes passed to uMIDI

static UMidiContext _midi;                   ///< state shared with the interrupt handlers
static UMidiSong _song;                      ///< song being played
static std::vector<UMidiBlock> _song_blocks; ///< storage of the blocks of the song being played
static std::vector<byte> _song_data;         ///< storage of the messages of the song being played
static byte _silence[16 * 3 * 3];            ///< messages silencing and resetting all channels
static byte _default_volumes[16 * 3];        ///< messages setting the volume of all channels to #DEFAULT_CHANNEL_VOLUME
static byte _volume;                         ///< music volume set by the user
static bool _volume_pending;                 ///< whether uMIDI has yet to get #_volume
static uint32 _volume_time;                  ///< #_realtime_tick when uMIDI last got the music volume

static FMusicDriver_MiNT iFMusicDriver_MiNT;

/**
 * Close the MIDI port, if open. Also registered with atexit(): left installed
 * after the program ended, the interrupt handlers would crash the system on the
 * next timer tick or MIDI byte received.
 */
static void CloseMidi()
{
	UMidiClose(&_midi);
}

/**
 * Pass the music volume to uMIDI, which then sends all 16 channel volumes, at
 * most every #VOLUME_INTERVAL ms: a dragged slider would flood the MIDI port.
 * @param force Pass a pending volume regardless of the interval.
 */
static void UpdateVolume(bool force)
{
	if (!_volume_pending) return;
	if (!force && _realtime_tick - _volume_time < VOLUME_INTERVAL) return;

	UMidiSetVolume(&_midi, _volume);
	_volume_pending = false;
	_volume_time = _realtime_tick;
}

/**
 * Append MIDI messages to song data in the form uMIDI expects: channel
 * messages with explicit status bytes and complete system exclusive messages.
 * Other system messages are left out, as are truncated messages and system
 * exclusive messages which would not fit into the FIFO.
 * @param data The song data to append to.
 * @param msg The messages to append; running status is allowed.
 * @param length The number of bytes in \a msg.
 */
static void AppendMessages(std::vector<byte> &data, const byte *msg, size_t length)
{
	const byte *end = msg + length;
	byte running_status = 0;

	while (msg < end) {
		byte status = *msg;
		if ((status & 0x80) != 0) {
			msg++;
		} else if (running_status != 0) {
			status = running_status;
		} else {
			/* data byte without a status byte to belong to */
			msg++;
			continue;
		}

		size_t data_bytes;
		switch (status & 0xF0) {
			case MIDIST_PROGCHG:
			case MIDIST_CHANPRESS:
				data_bytes = 1;
				break;

			case MIDIST_NOTEOFF:
			case MIDIST_NOTEON:
			case MIDIST_POLYPRESS:
			case MIDIST_CONTROLLER:
			case MIDIST_PITCHBEND:
				data_bytes = 2;
				break;

			default:
				/* real-time messages are left out and keep running status */
				if (status >= 0xF8) continue;
				/* other system messages cancel running status */
				running_status = 0;
				switch (status) {
					case MIDIST_SYSEX: {
						const byte *sysex_end = msg;
						while (sysex_end < end && (*sysex_end < 0x80 || *sysex_end >= 0xF8)) sysex_end++;
						if (sysex_end == end) return;
						if (*sysex_end != MIDIST_ENDSYSEX) {
							/* another status byte ends the message unfinished; it is left out */
							msg = sysex_end;
							break;
						}
						if ((size_t)(sysex_end - msg) + 2 < UMIDI_FIFO_SIZE) {
							data.push_back(MIDIST_SYSEX);
							data.insert(data.end(), msg, sysex_end + 1);
						}
						msg = sysex_end + 1;
						break;
					}
					case MIDIST_TC_QFRAME:
					case MIDIST_SONGSEL:
						msg = min(msg + 1, end);
						break;
					case MIDIST_SONGPOSPTR:
						msg = min(msg + 2, end);
						break;
					default:
						break;
				}
				continue;
		}

		if ((size_t)(end - msg) < data_bytes) return;
		running_status = status;
		data.push_back(status);
		data.insert(data.end(), msg, msg + data_bytes);
		msg += data_bytes;
	}
}

/**
 * Append a block of messages to a song.
 * @param blocks The blocks of the song.
 * @param data The song data.
 * @param time The playback time in microseconds the block is due at.
 * @param msg The messages of the block.
 * @param length The number of bytes in \a msg.
 */
static void AppendBlock(std::vector<UMidiBlock> &blocks, std::vector<byte> &data, uint32 time, const byte *msg, size_t length)
{
	AppendMessages(data, msg, length);
	UMidiBlock block = { time, (uint32)data.size() };
	blocks.push_back(block);
}

void MusicDriver_MiNT::PlaySong(const MusicSongInfo &song)
{
	DEBUG(driver, 2, "MiNT-MIDI: PlaySong: entry");

	MidiFile file;
	if (!file.LoadSong(song)) return;
	DEBUG(driver, 2, "MiNT-MIDI: PlaySong: loaded song");

	std::vector<UMidiBlock> blocks;
	std::vector<byte> data;

	/* Silence and reset the device, giving it some time to process each step. */
	size_t length;
	const byte *msg;
	AppendBlock(blocks, data, 0, _silence, lengthof(_silence));
	msg = MidiGetStandardSysexMessage(MidiSysexMessage::ResetGM, length);
	AppendBlock(blocks, data, RESET_STEP_TIME, msg, length);
	msg = MidiGetStandardSysexMessage(MidiSysexMessage::RolandSetReverb, length);
	AppendBlock(blocks, data, 2 * RESET_STEP_TIME, msg, length);
	/* Channel volumes as set by the reset, which uMIDI sends scaled by the user volume. */
	AppendBlock(blocks, data, 2 * RESET_STEP_TIME, _default_volumes, lengthof(_default_volumes));
	const uint32 first_block = (uint32)blocks.size();
	const uint32 song_start_time = 3 * RESET_STEP_TIME;

	/* Blocks from the end override on are not played. */
	size_t end_block = file.blocks.size();
	if (song.override_end > 0) {
		for (size_t i = 0; i < end_block; i++) {
			if (file.blocks[i].ticktime >= (uint32)song.override_end) {
				end_block = i;
				break;
			}
		}
	}

	/* A looping song continues at the first block from the start override on;
	 * a song played once starts there. */
	size_t start_block = 0;
	if (song.override_start > 0) {
		for (size_t i = 0; i < end_block; i++) {
			if (file.blocks[i].ticktime >= (uint32)song.override_start) {
				start_block = i;
				break;
			}
		}
	}

	uint32 skip_time = 0; // microseconds skipped at the beginning of the song
	for (size_t i = 0; i < end_block; i++) {
		const MidiFile::DataBlock &block = file.blocks[i];
		AppendBlock(blocks, data, block.realtime, block.data.Begin(), block.data.Length());

		if (i == start_block && start_block > 0 && !song.loop) {
			/* The blocks before the start are sent at once, as they may contain
			 * important controller and program changes. Start their transmission
			 * early enough to not delay the following blocks. */
			uint32 preload_time = (uint32)(data.size() - blocks[first_block - 1].end) * MICROSECONDS_PER_BYTE;
			skip_time = block.realtime > preload_time ? block.realtime - preload_time : 0;
			DEBUG(driver, 2, "MiNT-MIDI: PlaySong: start from block %d (ticktime %d, realtime %.3f ms, preload %.3f ms)", (int)i, (int)block.ticktime, block.realtime / 1000.0, preload_time / 1000.0);
		}
	}
	for (size_t i = first_block; i < blocks.size(); i++) {
		uint32 time = blocks[i].time;
		blocks[i].time = song_start_time + (time > skip_time ? time - skip_time : 0);
	}

	/* uMIDI's block times are timer periods, rounded up */
	for (size_t i = 0; i < blocks.size(); i++) {
		blocks[i].time = UMidiPeriods(blocks[i].time);
	}

	/* The interrupt must not read the song while it is replaced. */
	UMidiStop(&_midi);
	std::swap(blocks, _song_blocks);
	std::swap(data, _song_data);

	_song.blocks = _song_blocks.data();
	_song.data = _song_data.data();
	_song.numBlocks = (uint32)_song_blocks.size();
	_song.loopBlock = (song.loop && end_block > 0) ? first_block + (uint32)start_block : _song.numBlocks;
	DEBUG(driver, 2, "MiNT-MIDI: PlaySong: %d blocks, %d bytes, loop block %d", (int)_song.numBlocks, (int)_song_data.size(), (int)_song.loopBlock);

	UpdateVolume(true);
	UMidiPlay(&_midi, &_song, song.loop ? -1 : 0);
}

void MusicDriver_MiNT::StopSong()
{
	DEBUG(driver, 2, "MiNT-MIDI: StopSong: entry");
	UMidiStop(&_midi);
}

bool MusicDriver_MiNT::IsSongPlaying()
{
	UpdateVolume(false);
	return UMidiIsPlaying(&_midi) != 0;
}

void MusicDriver_MiNT::SetVolume(byte vol)
{
	_volume = vol;
	_volume_pending = true;
	UpdateVolume(false);
}

const char *MusicDriver_MiNT::Start(const char * const *parm)
{
	if (!UMidiOpen(&_midi)) return "MIDI port not available";

	for (uint ch = 0; ch < 16; ch++) {
		byte *msg = &_silence[ch * 9];
		msg[0] = MIDIST_CONTROLLER | ch;
		msg[1] = MIDICT_MODE_ALLSOUNDOFF;
		msg[2] = 0;
		msg[3] = MIDIST_CONTROLLER | ch;
		msg[4] = MIDICT_MODE_ALLNOTESOFF;
		msg[5] = 0;
		msg[6] = MIDIST_CONTROLLER | ch;
		msg[7] = MIDICT_MODE_RESETALLCTRL;
		msg[8] = 0;

		msg = &_default_volumes[ch * 3];
		msg[0] = MIDIST_CONTROLLER | ch;
		msg[1] = MIDICT_CHANVOLUME;
		msg[2] = DEFAULT_CHANNEL_VOLUME;
	}

	static bool atexit_registered = false;
	if (!atexit_registered) atexit_registered = atexit(CloseMidi) == 0;

	DEBUG(driver, 2, "MiNT-MIDI: Start: MIDI port open");
	return NULL;
}

void MusicDriver_MiNT::Stop()
{
	CloseMidi();

	std::vector<UMidiBlock>().swap(_song_blocks);
	std::vector<byte>().swap(_song_data);

	DEBUG(driver, 2, "MiNT-MIDI: Stop: MIDI port closed");
}
