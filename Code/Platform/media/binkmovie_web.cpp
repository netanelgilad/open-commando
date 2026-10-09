/*
** BINKMovie on an HTML <video> overlay.
**
** The retail Bink videos (Data/Movies/*.bik) are transcoded offline to WebM
** (tools/transcode-movies.sh) and served next to the game as
** movies/<name>.webm. Play() asks the page to show the video over the canvas;
** the movie is complete when the video ends, errors (no transcode yet), or
** the player presses a key. Subtitles would come from data\subtitle.ini,
** which the retail data doesn't ship, so there are none to show.
*/
#include "BINKMovie.h"

#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, Rebel_JS_Movie_Play, (const char *path), {
  // no backslashes here: EM_JS bodies pass through a C string literal
  var name = UTF8ToString(path).split(String.fromCharCode(92)).join('/').split('/').pop();
  var dot = name.lastIndexOf('.');
  if (dot > 0 && name.slice(dot).toLowerCase() === '.bik') name = name.slice(0, dot);
  Module.rebelMovieDone = false;
  if (typeof Module.rebelPlayMovie !== 'function') {
    Module.rebelMovieDone = true;
    return;
  }
  Module.rebelPlayMovie(name, function() { Module.rebelMovieDone = true; });
});
EM_JS(void, Rebel_JS_Movie_Stop, (void), {
  if (typeof Module.rebelStopMovie === 'function') Module.rebelStopMovie();
  Module.rebelMovieDone = true;
});
EM_JS(int, Rebel_JS_Movie_Done, (void), { return Module.rebelMovieDone === false ? 0 : 1; });
#else
static void Rebel_JS_Movie_Play(const char *) {}
static void Rebel_JS_Movie_Stop() {}
static int Rebel_JS_Movie_Done() { return 1; }
#endif

static bool Playing = false;

void BINKMovie::Init() {}
void BINKMovie::Shutdown() { Stop(); }

void BINKMovie::Play(const char *filename, const char *, FontCharsClass *) {
  Playing = true;
  Rebel_JS_Movie_Play(filename ? filename : "");
}

void BINKMovie::Stop() {
  if (Playing)
    Rebel_JS_Movie_Stop();
  Playing = false;
}

void BINKMovie::Update() {
  if (Playing && Rebel_JS_Movie_Done())
    Playing = false;
}

void BINKMovie::Render() {} // the <video> element draws itself

bool BINKMovie::Is_Complete() { return !Playing || Rebel_JS_Movie_Done(); }
