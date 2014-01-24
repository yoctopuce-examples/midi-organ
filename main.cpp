// This is the main file for our DIY MIDI Organ (wifi-driven)
//
// Compile it with Midifile library from Craig Stuart Sapp
//
// More info on http://www.yoctopuce.com/EN/article/building-a-midi-automatic-organ

#include <iostream>
#include "MidiFile.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "yocto_api.h"
#include "yocto_relay.h"

typedef struct {
    YRelay  *relay;
    int     key;
    long    busyUntil;      // used during play
} Pipe;

typedef struct {
    Pipe    *pipe;
    double  weight;         // total duration * velocity
} Key;

typedef struct {
    int     channel;
    int     time;           // in milliseconds
    int     key;            // midi note index
    int     duration;       // in milliseconds
    double  velocity;       // in range [0.0 - 1.0]
} Note;

int     basenote = 43;      // midi note assigned to lowest opipe
double  tempo = 120.0;      // default tempo is 120 beats per minute
double  timebase = 0.0;

Pipe    pipes[32];
int     npipes = 0;
int     volume[17];         // volume for each channel (0 to mute, 1...10 = current volume)
double  totalw = 0.0;
Key     keys[128];
vector<Note> notes;

bool noteSort(const Note &i, const Note &j)
{
    return i.time < j.time;
}

void showQuality(const char *context)
{
    double weight = 0.0;
    for(int i = 0; i < 128; i++) {
        if(keys[i].pipe) {
            weight += keys[i].weight;
        }
    }
    printf("%s: %.1f%% notes can be played\n", context, 100*weight/totalw);
}

int main(int argc, const char * argv[])
{
    unsigned    i, j;
    string      errmsg;
    int         applyvol = 0;
    int         verbose = 0;
    int         mute = 0;
    int         maxtune = 99;
    int         tempoLogged = 0;
    const char  *organIP = "192.168.1.71";
    
    // Initialize arrays
    memset(keys, 0, sizeof(keys));
    memset(pipes, 0, sizeof(pipes));
    for(i = 1; i <= 16; i++) {
        // setup default volume (mute percussion channel 10)
        volume[i] = (i == 10 ? 0 : 10);
    }
    
    if(argc < 2) {
        cerr << "Usage: yoctopipe <file.midi> [@<tempo>] [<channel1-16>:<volume0-10>] [-ip:x.x.x.x] [-dyn] [-mute] [-verbose] [-max:#]" << endl;
        exit(1);
    }
    // Parse optional arguments (tempo/volume)
    for(i = 2; i < argc; i++) {
        const char *p = argv[i];
        if(*p == '@') {
            tempo = atof(p+1);
        } else if(*p == '-') {
            if(!strcmp(p+1,"verbose")) verbose = 1;
            else if(!strcmp(p+1,"dyn")) applyvol = 1;
            else if(!strcmp(p+1,"mute")) mute = 1;
            else if(!strncmp(p+1,"max:",4)) maxtune = atoi(p+5);
            else if(!strncmp(p+1,"ip:",3)) organIP = p+4;
        } else if(isdigit(*p)) {
            while(isdigit(*p)) p++;
            if(*p == ':') {
                j = atoi(argv[i]);
                if(j >= 1 && j <= 16) {
                    volume[j] = atof(p+1);
                }
            }
        }
    }

    // Connect to the Yocto-Organ, find available notes
    YAPI::RegisterHub(organIP, errmsg);
    YRelay  *relay = YRelay::FirstRelay();
    while(relay) {
        string name = relay->get_logicalName();
        if(name.find("tune") == 0) {
            int tune = atoi(name.c_str()+4);
            if(tune <= maxtune) {
                pipes[npipes].relay = relay;
                pipes[npipes++].key = tune;
            } else {
                printf("Dropping pipe %02d\n", tune);
            }
        }
        relay = relay->nextRelay();
    }

    // Load midi file, convert into a simple vector of notes
    MidiFile midifile(argv[1]);
    midifile.absoluteTime();
    midifile.joinTracks();
    double timebase = 60000.0 / tempo / midifile.getTicksPerQuarterNote();
    int oldticks = 0;
    int currtime = 0;
    for (i = 0; i < midifile.getNumEvents(0); i++) {
        MFEvent &iEvent = midifile.getEvent(0, i);
        int channel = iEvent.getChannelNibble()+1;
        if (iEvent.isTimbre() && volume[channel] != 0) {
            // Instrument change
            if(iEvent.data[1] >= 112) {
                // silence channel when using percussion instrument
                if(volume[channel] > 0) volume[channel] = -volume[channel];
            } else {
                if(volume[channel] < 0) volume[channel] = -volume[channel];
            }
        } else if((iEvent.data[0] & 0xf0) == 0xb0  && volume[channel] > 0 && iEvent.data[1] == 7) {
            // Volume change
            volume[channel] = iEvent.data[2] * 10 / 127;
        } else if(iEvent.data.getSize() > 3 && iEvent.data[0] == 0xff && iEvent.data[1] == 0x03) {
            // Track name MIDI meta event
            channel = j = 0;
            while(j < midifile.getNumEvents(0) && channel == 0) {
                MFEvent &jEvent = midifile.getEvent(0, j++);
                if(jEvent.track == iEvent.track && (jEvent.data[0] & 0xf0) != 0xf0) {
                    channel = jEvent.getChannelNibble()+1;
                }
            }
            cout << "Channel " << channel << ": ";
            for (j = 3; j<iEvent.data.getSize(); j++) {
                cout << (char)iEvent.data[j];
            }
            cout << "\n";
        } else if (iEvent.isTempo()) {
            // New tempo
            int usecs = (iEvent.data[3] << 16) + (iEvent.data[4] << 8) + (iEvent.data[5] << 0);
            tempo = 60000000.0 / usecs;
            timebase = 60000.0 / tempo / midifile.getTicksPerQuarterNote();
            if(!tempoLogged) {
                cout << "Tempo: " << tempo << "/min" << endl;
                tempoLogged = 1;
            }
        } else if (iEvent.isNoteOn() && volume[channel] > 0) {
            // search for end of note
            Note note;
            note.channel  = channel;
            note.time     = currtime;
            note.key      = iEvent.data[1];
            note.velocity = iEvent.data[2] * volume[channel] / 1270.0;
            note.duration = 0;
            j = i + 1;
            while(j < midifile.getNumEvents(0) && note.duration == 0) {
                MFEvent &jEvent = midifile.getEvent(0, j++);
                if (jEvent.isNoteOff()) {
                    if(jEvent.track == iEvent.track && jEvent.data[1] == note.key) {
                        note.duration = (int)((jEvent.time - iEvent.time) * timebase);
                        if(note.duration > 0) {
                            notes.push_back(note);
                        }
                    }
                }
            }
        }
        currtime += floor((iEvent.time - oldticks) * timebase + 0.5);
        oldticks = iEvent.time;
    }
    if(notes.size() == 0) {
        cerr << "Could not load any note !" << endl;
        exit(1);
    }
    
    // Now count the use of each keys
    for(i = 0; i < notes.size(); i++) {
        Note &note = notes.at(i);
        keys[note.key].weight += note.duration * note.velocity;
    }
    for(i = 0; i < 128; i++) {
        totalw += keys[i].weight;
    }

    // Search for the best match between notes and pipes
    double baseweight = 0;
    for(i = 0; i < 127; i++) {
        double weight = 0;
        for(j = 0; j < npipes; j++) {
            if(i+pipes[j].key < 128) {
                weight += keys[i+pipes[j].key].weight;
            }
        }
        if(baseweight < weight) {
            baseweight = weight;
            basenote = i;
        }
    }
    
    // Assign pipes to keys
    for(i = 0; i < npipes; i++) {
        if(basenote+pipes[j].key < 128) {
            keys[basenote+pipes[i].key].pipe = pipes+i;
        }
    }
    showQuality("Without harmonics");
    
    // Assign default notes for missing notes
    for(i = 0; i < 128-12; i++) {
        if(!keys[i+12].pipe && keys[i].pipe) {
            keys[i+12].pipe = keys[i].pipe;
        }
    }
    for(i = 127; i >= 12; i--) {
        if(!keys[i-12].pipe && keys[i].pipe) {
            keys[i-12].pipe = keys[i].pipe;
        }
    }
    showQuality("With octaves");
    for(i = 0; i < 128-7; i++) {
        if(!keys[i+7].pipe && keys[i].pipe) {
            keys[i+7].pipe = keys[i].pipe;
        }
    }
    showQuality("With fifths");
    for(i = 0; i < 128-16; i++) {
        if(!keys[i+16].pipe && keys[i].pipe) {
            keys[i+16].pipe = keys[i].pipe;
        }
    }
    showQuality("With thirds");
    
    if(applyvol) {
        // Reduce notes duration based on velocity
        double maxVelocity = notes.at(i).velocity;
        for(i = 1; i < notes.size(); i++) {
            Note &note = notes.at(i);
            if(maxVelocity < note.velocity) {
                maxVelocity = note.velocity;
            }
        }
        for(i = 0; i < notes.size(); i++) {
            Note &note = notes.at(i);
            note.duration *= note.velocity / maxVelocity;
        }
    }
    
    // Further reduce notes to make sure each note is played
    for(i = 0; i < notes.size(); i++) {
        Note &note = notes.at(i);
        Pipe *pipe = keys[note.key].pipe;
        if(pipe) {
            int endTime = note.time + note.duration;
            j = i +1;
            while(j < notes.size()) {
                Note &nextNote = notes.at(j++);
                if(nextNote.time >= endTime+10) {
                    break;
                }
                if(keys[nextNote.key].pipe == pipe) {
                    if(nextNote.time > note.time+20 && nextNote.velocity > note.velocity) {
                        // shorten note to replay later & louder one
                        note.duration = nextNote.time - note.time - 10;
                        if(nextNote.time + nextNote.duration + 20 < endTime) {
                            nextNote.duration = endTime - nextNote.time;
                        }
                        break;
                    } else {
                        // cancel duplicate note
                        nextNote.duration = 0;
                    }
                }
            }
        }
    }
    
    // Play the notes
    int firstnote = 0;
    long startTime = YAPI::GetTickCount() + 100 - notes.at(firstnote).time;
    int channel = -1;
    for(i = firstnote; i < notes.size(); i++) {
        const Note &note = notes.at(i);
        // Ignore cancelled notes
        if(note.duration <= 0) continue;
        // Make sure we have a pipe for that note
        Pipe *pipe = keys[note.key].pipe;
        if(pipe) {
            long waitTime = pipe->busyUntil - (long)YAPI::GetTickCount();
            if(waitTime > 0) {
                // Still playing a note, need to wait               
                printf("(%d)",(int)waitTime);
                fflush(stdout);
                YAPI::Sleep((unsigned)waitTime, errmsg);
            } else {
                // Give up to 25ms anyway to flush prev commands in case
                int flushTime = (int)(startTime + note.time - YAPI::GetTickCount() - 4);
                if(flushTime > 25) flushTime = 25;
                if(flushTime > 0) {
                    YAPI::Sleep(flushTime,errmsg);
                }
            }
            // Next note can now be scheduled
            int duration = note.duration;
            waitTime = startTime + note.time - YAPI::GetTickCount();
            if(waitTime < 0) {
                // Oops, we are late
                printf("(%d)", (int)waitTime);
                startTime = YAPI::GetTickCount() - note.time;
                waitTime = 0;
            }
            if(note.duration > 0) {
                pipe->busyUntil = startTime + note.time + note.duration;
                if(!mute) {
                    pipe->relay->delayedPulse((int)waitTime, duration);
                }
                if(note.channel != channel) {
                    channel = note.channel;
                    if(verbose) {
                        printf("\n#%d:", channel);
                    }
                }
                if(verbose) {
                    printf("[%d(%d):%d@%.0f%%]", note.key, basenote+pipe->key, note.duration, 100*note.velocity);
                }
            }
            fflush(stdout);
        } else if(note.key) {
            printf("[/%d]", note.key);
            fflush(stdout);
        }
    }
    
    return 0;
}

