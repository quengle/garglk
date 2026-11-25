/*
 * Copyright (C) 2026  the Quengle authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * os_glk_quengle.cpp: quengle's side of the autosave (what scarier-autosave.mm is for spatterlight).
 *
 * one file, /autosave/scarier.sav, through glk streams (emglken has no filesystem; the host keeps what's written
 * under /autosave/). it holds the generators' state, then the engine container os_glk_autosave.cpp hands over,
 * deflated: an adrift 5 save is the whole game as xml (1.7 MB for lost coastlines), written every turn.
 * no glk library archive: the windows open as on a fresh start and the host replays the screen, so
 * scarier_autosave_restore_library only puts the generators back, and restarts what the adrift 5 sound channels
 * were playing (spatterlight resumes those from its own snapshot). a sound still marked playing starts over.
 */

#include "os_glk_internal.h"

#ifdef QUENGLE

extern "C" {
#include "glkstart.h"
}

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <zlib.h>

static const char *const QUENGLE_AUTOSAVE_PATH = "/autosave/scarier.sav";
static const char *const QUENGLE_AUTOSAVE_MAGIC = "QSCARIER1\n";

/* the generators read with the game, applied by restore_library */
static ScarierGlkFrontendState quengle_saved_rng;
static int quengle_saved_loop[GSC_A5_MAX_CHANNELS];

int gsc_a5_chan_loop[GSC_A5_MAX_CHANNELS];

static frefid_t
quengle_autosave_fref (void)
{
  return glkunix_fileref_create_by_name_uncleaned (
      fileusage_SavedGame | fileusage_BinaryMode, (char *) QUENGLE_AUTOSAVE_PATH, 0);
}

bool
scarier_autosave_exists (void)
{
  frefid_t fref = quengle_autosave_fref ();
  bool exists = fref != nullptr && glk_fileref_does_file_exist (fref);

  if (fref != nullptr)
    glk_fileref_destroy (fref);
  return exists;
}

/* spatterlight skips saves after arrange/redraw events and has an off switch; here every prompt is worth it */
bool
scarier_autosave_wanted (void)
{
  return true;
}

bool
scarier_autosave_enabled (void)
{
  return true;
}

void
scarier_autosave_discard (void)
{
  frefid_t fref = quengle_autosave_fref ();

  if (fref == nullptr)
    return;
  if (glk_fileref_does_file_exist (fref))
    glk_fileref_delete_file (fref);
  glk_fileref_destroy (fref);
}

void
scarier_autosave_write (const std::string &engine_state)
{
  ScarierGlkFrontendState st;
  std::ostringstream rng;
  std::string out;
  frefid_t fref;
  strid_t stream;
  int i;

  gsc_stash_frontend_state (&st);
  rng << st.rng_usenative;
  for (i = 0; i < 4; i++)
    rng << ' ' << st.rng_state[i];
  rng << ' ' << st.rng_runner;
  for (i = 0; i < 4; i++)
    rng << ' ' << st.rng_runner_state[i];
  rng << ' ' << st.rng_runner_draws;
  for (i = 0; i < GSC_A5_MAX_CHANNELS; i++)
    rng << ' ' << (gsc_is_a5 ? gsc_a5_chan_sound[i] : 0) << ' ' << gsc_a5_chan_loop[i];
  rng << '\n';

  out = QUENGLE_AUTOSAVE_MAGIC;
  out += rng.str ();
  {
    uLongf packed_length = compressBound ((uLong) engine_state.size ());
    std::string packed (packed_length, '\0');

    if (compress2 ((Bytef *) &packed[0], &packed_length, (const Bytef *) engine_state.data (),
                   (uLong) engine_state.size (), Z_BEST_SPEED) != Z_OK)
      return;
    out += std::to_string ((unsigned long) engine_state.size ());
    out += '\n';
    out.append (packed, 0, packed_length);
  }

  fref = quengle_autosave_fref ();
  if (fref == nullptr)
    return;
  stream = glk_stream_open_file (fref, filemode_Write, 0);
  glk_fileref_destroy (fref);
  if (stream == nullptr)
    return;
  glk_put_buffer_stream (stream, (char *) out.data (), (glui32) out.size ());
  glk_stream_close (stream, nullptr);
}

bool
scarier_autosave_read_game (std::string *out)
{
  const std::string magic = QUENGLE_AUTOSAVE_MAGIC;
  frefid_t fref = quengle_autosave_fref ();
  strid_t stream;
  std::string data;
  char buffer[16384];
  glui32 got;
  size_t eol;
  int i;

  if (fref == nullptr)
    return false;
  stream = glk_fileref_does_file_exist (fref)
           ? glk_stream_open_file (fref, filemode_Read, 0) : nullptr;
  glk_fileref_destroy (fref);
  if (stream == nullptr)
    return false;
  while ((got = glk_get_buffer_stream (stream, buffer, sizeof buffer)) > 0)
    data.append (buffer, got);
  glk_stream_close (stream, nullptr);

  if (data.compare (0, magic.size (), magic) != 0)
    return false;
  eol = data.find ('\n', magic.size ());
  if (eol == std::string::npos)
    return false;
  {
    std::istringstream rng (data.substr (magic.size (), eol - magic.size ()));
    ScarierGlkFrontendState st;

    rng >> st.rng_usenative;
    for (i = 0; i < 4; i++)
      rng >> st.rng_state[i];
    rng >> st.rng_runner;
    for (i = 0; i < 4; i++)
      rng >> st.rng_runner_state[i];
    rng >> st.rng_runner_draws;
    if (rng.fail ())
      return false;
    for (i = 0; i < GSC_A5_MAX_CHANNELS; i++)
      rng >> st.a5_chan_sound[i] >> quengle_saved_loop[i];
    if (rng.fail ())
      return false;
    quengle_saved_rng = st;
  }
  {
    size_t size_eol = data.find ('\n', eol + 1);
    unsigned long size;
    uLongf unpacked_length;

    if (size_eol == std::string::npos)
      return false;
    size = strtoul (data.c_str () + eol + 1, nullptr, 10);
    out->assign (size, '\0');
    unpacked_length = (uLongf) size;
    if (uncompress ((Bytef *) &(*out)[0], &unpacked_length, (const Bytef *) data.data () + size_eol + 1,
                    (uLong) (data.size () - size_eol - 1)) != Z_OK || unpacked_length != size)
      return false;
  }
  return true;
}

/*
 * undo snapshots as line diffs. consecutive a5 snapshots differ in a few dozen of ~15k lines (lost coastlines:
 * 1.3 KB of 269 KB), and deflate's 32 KB window can't see that across whole snapshots. stored forms:
 *   "QD1\n" then ops: "C<first> <count>\n" copies base lines, "A<bytes>\n<bytes>" adds whole lines
 *   "QR1\n" then the target itself (the diff didn't round-trip, or there was nothing to gain)
 * a line keeps its '\n'; only the last may lack one.
 */
static void
quengle_split_lines (const std::string &s, std::vector<std::pair<size_t, size_t> > *lines)
{
  size_t start = 0;

  while (start < s.size ())
    {
      size_t eol = s.find ('\n', start);
      size_t end = eol == std::string::npos ? s.size () : eol + 1;

      lines->push_back (std::make_pair (start, end - start));
      start = end;
    }
}

bool
quengle_diff_decode (const std::string &base, const std::string &stored, std::string *target)
{
  std::vector<std::pair<size_t, size_t> > lines;
  size_t pos = 4;

  if (stored.compare (0, 4, "QR1\n") == 0)
    {
      target->assign (stored, 4, std::string::npos);
      return true;
    }
  if (stored.compare (0, 4, "QD1\n") != 0)
    return false;
  quengle_split_lines (base, &lines);
  target->clear ();
  while (pos < stored.size ())
    {
      size_t eol = stored.find ('\n', pos);
      char op = stored[pos];

      if (eol == std::string::npos)
        return false;
      if (op == 'C')
        {
          char *rest;
          unsigned long first = strtoul (stored.c_str () + pos + 1, &rest, 10);
          unsigned long count = strtoul (rest, nullptr, 10);

          if (first > lines.size () || count > lines.size () - first)
            return false;
          for (unsigned long k = first; k < first + count; k++)
            target->append (base, lines[k].first, lines[k].second);
          pos = eol + 1;
        }
      else if (op == 'A')
        {
          unsigned long bytes = strtoul (stored.c_str () + pos + 1, nullptr, 10);

          if (bytes > stored.size () - (eol + 1))
            return false;
          target->append (stored, eol + 1, bytes);
          pos = eol + 1 + bytes;
        }
      else
        return false;
    }
  return true;
}

static std::string quengle_diff_compute (const std::string &base, const std::string &target);

/* every turn diffs the whole stack, but only the newest link is new: the rest come from here */
std::string
quengle_diff_encode (const std::string &base, const std::string &target)
{
  static std::unordered_map<std::string, std::string> cache;
  std::hash<std::string> hash;
  std::string key = std::to_string (hash (base)) + ' ' + std::to_string (base.size ()) + ' '
                    + std::to_string (hash (target)) + ' ' + std::to_string (target.size ());
  auto found = cache.find (key);

  if (found != cache.end ())
    return found->second;
  if (cache.size () > 64)
    cache.clear ();
  return cache[key] = quengle_diff_compute (base, target);
}

static std::string
quengle_diff_compute (const std::string &base, const std::string &target)
{
  std::vector<std::pair<size_t, size_t> > bl, tl;
  std::unordered_map<std::string, std::vector<size_t> > where;
  std::string out = "QD1\n", added, check;
  size_t t = 0, bp = 0;

  quengle_split_lines (base, &bl);
  quengle_split_lines (target, &tl);
  for (size_t k = 0; k < bl.size (); k++)
    where[base.substr (bl[k].first, bl[k].second)].push_back (k);

  auto same = [&] (size_t b, size_t tt) {
    return bl[b].second == tl[tt].second
           && base.compare (bl[b].first, bl[b].second, target, tl[tt].first, tl[tt].second) == 0;
  };
  auto run_from = [&] (size_t b, size_t tt) {
    size_t n = 0;
    while (b + n < bl.size () && tt + n < tl.size () && same (b + n, tt + n))
      n++;
    return n;
  };
  auto flush_added = [&] () {
    if (added.empty ())
      return;
    out += 'A';
    out += std::to_string ((unsigned long) added.size ());
    out += '\n';
    out += added;
    added.clear ();
  };

  while (t < tl.size ())
    {
      size_t best = 0, best_at = 0;

      if (bp < bl.size () && same (bp, t))
        {
          best = run_from (bp, t);
          best_at = bp;
        }
      else
        {
          /* the nearest few places this line occurs in the base, at or after where we are, then before */
          auto found = where.find (target.substr (tl[t].first, tl[t].second));

          if (found != where.end ())
            {
              const std::vector<size_t> &at = found->second;
              size_t from = (size_t) (std::lower_bound (at.begin (), at.end (), bp) - at.begin ());

              for (size_t k = from; k < at.size () && k < from + 4; k++)
                {
                  size_t n = run_from (at[k], t);
                  if (n > best) { best = n; best_at = at[k]; }
                }
              for (size_t k = from; k > 0 && k + 4 > from; k--)
                {
                  size_t n = run_from (at[k - 1], t);
                  if (n > best) { best = n; best_at = at[k - 1]; }
                }
            }
          /* a lone common line ("</Object>") costs more as a copy than as text */
          if (best < 2 && t + best < tl.size ())
            best = 0;
        }
      if (best == 0)
        {
          added.append (target, tl[t].first, tl[t].second);
          t++;
          continue;
        }
      flush_added ();
      out += 'C';
      out += std::to_string ((unsigned long) best_at);
      out += ' ';
      out += std::to_string ((unsigned long) best);
      out += '\n';
      t += best;
      bp = best_at + best;
    }
  flush_added ();

  if (out.size () >= target.size () || !quengle_diff_decode (base, out, &check) || check != target)
    return "QR1\n" + target;
  return out;
}

bool
scarier_autosave_restore_library (void)
{
  int ch;

  gsc_recover_frontend_state (&quengle_saved_rng);
  if (!gsc_is_a5 || !gsc_a5_sound_ok)
    return true;
  for (ch = 1; ch < GSC_A5_MAX_CHANNELS; ch++)
    {
      glui32 snd = quengle_saved_rng.a5_chan_sound[ch];

      if (snd == 0)
        continue;
      if (gsc_a5_channels[ch] == nullptr)
        gsc_a5_channels[ch] = glk_schannel_create ((glui32) ch);
      /* as gsc_a5_sound_event plays it */
      if (gsc_a5_channels[ch] != nullptr
          && glk_schannel_play_ext (gsc_a5_channels[ch], snd,
                                    quengle_saved_loop[ch] ? 0xffffffffu : 1, (glui32) ch))
        {
          gsc_a5_chan_sound[ch] = snd;
          gsc_a5_chan_loop[ch] = quengle_saved_loop[ch];
        }
    }
  return true;
}

#endif /* QUENGLE */
