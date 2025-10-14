#pragma once

void sig_handler(int signum);

void switch_pipeline_source(const char * source_type, const char * source_path);
void fast_forward(double rate);
void fast_rewind(double rate);
void skip_duration(int64_t skip_ms);
void normal_playback();
void pause_playback();
void resume_playback();

/* --- Console arguments parser --- */
#define __BeginParseConsoleArguments__(printHelpFunction) \
  if (argc < 1 || (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "/?") \
  || !strcmp(argv[1], "/h")))) { printHelpFunction(); return 1; } \
  for (int ArgID = 1; ArgID < argc; ArgID++) { const char* Arg = argv[ArgID];

#define __OnArgument(Name) if (!strcmp(Arg, Name))
#define __ArgValue (argc > ArgID + 1 ? argv[++ArgID] : "")
#define __EndParseConsoleArguments__ else { printf("ERROR: Unknown argument %s\n",Arg); return 1; } }