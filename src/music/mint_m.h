/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file mint_m.h Base for music playback through the MIDI port of Atari computers. */

#ifndef MUSIC_MINT_H
#define MUSIC_MINT_H

#include "music_driver.hpp"

/** The music player for the MIDI port of Atari computers running FreeMiNT. */
class MusicDriver_MiNT : public MusicDriver {
public:
	/* virtual */ const char *Start(const char * const *param);

	/* virtual */ void Stop();

	/* virtual */ void PlaySong(const MusicSongInfo &song);

	/* virtual */ void StopSong();

	/* virtual */ bool IsSongPlaying();

	/* virtual */ void SetVolume(byte vol);
	/* virtual */ const char *GetName() const { return "mint"; }
};

/** Factory for the music player for the MIDI port of Atari computers. */
class FMusicDriver_MiNT : public DriverFactoryBase {
public:
	FMusicDriver_MiNT() : DriverFactoryBase(Driver::DT_MUSIC, 5, "mint", "Atari MIDI Port Music Driver") {}
	/* virtual */ Driver *CreateInstance() const { return new MusicDriver_MiNT(); }
};

#endif /* MUSIC_MINT_H */
