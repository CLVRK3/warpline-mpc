#!/usr/bin/env python3
"""Writes Content/Genres.txt: the GENRE picker's tags for the factory patches.

Each line is `<patch file name>|<genre>,<genre>` (a patch can sit in several genres). The genre names must match
kGenres in src/Warpline.cpp. Every factory patch needs at least one; run this after adding patches:

    python3 tools/genres.py            (from the repo root; fails if a patch has no genre or a name is unknown)

Your own patches can be tagged the same way in <drive>/Warpline/Genres.txt.
"""
import os
import sys

GENRES = ["Trap / Rage", "Hip-Hop", "R&B / Soul", "EDM", "House / Techno", "Pop", "Synthwave / Retro",
          "Ambient / Cinematic", "Lo-Fi / Chill"]

# genre -> patch names (without the type code and .vital)
TAGS = {
    "Trap / Rage": """Rage Supersaw Chords; Opium Chords; Distorted Chord Stab; Rage Gate Chords; Hyper Detune Chords; Bitcrush Chords;
        Cloud Rage Chords; Rage Chord Pluck; Rage Anthem Lead; Hyper Detune Lead; Clipped Saw Lead; Square Rage Lead;
        Pitch Snap Lead; Distorted Brass Lead; Punch Saw Pluck; Rage Pluck; Snap Pluck; Clipped Pluck; Detuned Stab Pluck;
        Hyper Pluck; Vamp Organ; Sky Arp Lead; Punk Fuzz Lead; Slime Reese Bass; Chaos Screech; Gated Hyper Saw;
        Overdrive Stab; Glitch Crush Lead; Wavy Saw Lead; Dreamy Rage Pluck; 808 Glide; Clip 808; Fold 808; Growl 808; Rage 808; Sub Knock 808; Sub Pressure; Rubber Sub; Rage Bell;
        Tonka Chime; Ice Toll; Lux Toll; Wraith Bell; Church Bell; Music Box; Glockenspiel; Rage Saw Lead; Siren Wail; Dive Lead;
        Fuzz Synth Lead; Hyper Glide Lead; Distorted Saw Lead; Hyper Saw; Fuzz Lead; Screamer; Portamento Sine; Dark Choir;
        Void Organ; Ghost Strings; Haze Vox Pad; Night Swell; Dark Matter; Dark Strings Pad; Night Pad; Choral Pad; Aah Choir;
        Monk Chant; Ghost Vowels; Dark Pulse Seq; Flute Synth; Ocarina; Pizzicato Strings; Synth Harp; Harp Glint; Koto Pluck;
        Sitar Pluck; Kalimba Drop; Cello Bow; Low Strings; Tubular Chime; Bowl Resonance; Snare Crack; Rim Click; Closed Hat;
        Clap Room""",
    "Hip-Hop": """Opium Chords; Vamp Organ; Slime Reese Bass; Wavy Saw Lead; Dreamy Rage Pluck; Sky Arp Lead; 808 Glide; Sub Pressure; Rubber Sub; Moog Thump; Liquid Sub; Talking Bass; G-Funk Lead; Portamento Sine;
        Soft Sine Lead; Whistle Lead; Rhodes Hip Hop; Rhodes Classic; Rhodes Lofi Tape; Rhodes Mark Drive; Rhodes Suitcase;
        Lo-Fi Keys; Upright Felt; Analog Piano; Clav Funk; Wurly Drive; Muted Trumpet; Funk Brass Stab; Brass Section;
        Sax Synth; Muted Guitar; Nylon Pluck; Guitar Pick; Pizz Pluck; String Ensemble; Chamber Strings; Violin Solo;
        Cinematic Swell; Ooh Choir; Talk Box; Vocoder Lead; Yah Stab; Synth Kick; Snare Crack; Clap Room; Closed Hat; Rim Click;
        Conga Tone; Cowbell 80s; Church Bell; Music Box; Marimba Wood; Vibraphone Soft; Toy Piano; Kalimba Drop""",
    "R&B / Soul": """Silk Rhodes; Late Night Rhodes; Slow Jam Rhodes; Velvet Tines; Tape Soul Rhodes; Rhodes Neo Soul;
        Rhodes Mellow Jazz; Rhodes Dark Velvet; Rhodes Chorus Wide; Rhodes Phase Ballad; Rhodes Stage 73; Rhodes Classic; Rhodes Bright Bark;
        Rhodes Dream; Rhodes Bell Tines; Rhodes Echo Room; Rhodes Vintage Trem; Rhodes Wurli Hybrid; Rhodes Bell Mix;
        Dream Rhodes; EP Tines; Velvet Keys; Warm EP Pad Keys; Gospel Organ; Organ Drawbars; Satin Pad; Quiet Storm Pad;
        Neo Soul Strings; Warm Vocal Pad; Harmonic Bloom; Silk Pad; Warm Blanket; Breathing Pad; Falsetto Whistle;
        Smooth Sine Lead; Talk Box Lead; Soft Square Lead; Glide Pulse Lead; Silk Lead; Soft Sine Lead; Whistle Lead;
        Smooth Sub; Funk Synth Bass; Round Moog Bass; Neo Soul Sub; Slap Synth Bass; Liquid Sub; Vibraphone Soft;
        Vibes Tremolo; Ooh Choir; Choir Swell; Whisper Pad; Talk Box; Sax Synth; Soft French Horn; Muted Trumpet""",
    "EDM": """Hyper Detune Chords; Rage Anthem Lead; Punch Saw Pluck; Detuned Stab Pluck; Gated Hyper Saw; Chaos Screech; Festival Supersaw; Big Room Lead; Uplift Trance Lead; Trance Lead; Future Bass Saw; Hardstyle Screech;
        Anthem Supersaw; Bright Saw Lead; Hoover Lead; Sync Lead; Sync Scream; Screamer; Crystal Lead; Formant Lead;
        Riddim Growl; Bounce Bass; Electro Reese; Dub Wobble; Dubstep Yoi; FM Growl; Formant Growler; Formant Wobble;
        Neuro Grind; Reese Pressure; Wobble Classic; Fat Unison Bass; Saw Stack Bass; Sync Snarl; Foldback Bass; Diode Bite;
        Drop Pluck; Future Pluck; Island Pluck; Tropical Pluck; Progressive Pluck; Chord Stab; Sync Pluck; Bright Saw Pluck;
        Lush Pluck; Pluck Arp Glass; Euphoric Pad; Sidechain Pad; Breakdown Pad; PWM Air Pad; Filter Rise Pad; Shimmer Pad;
        Bright Poly Pad; Lush Supersaw Pad; Gated Saws; Trance Gate Pad; Sidechain Stabs; Saw Sequence; Crystal Steps;
        Ping Pong Bells; Triplet Motion; Big Synth Horns; Riser Sweep; White Noise Riser; Downlifter; Laser Zap; Tape Stop;
        Glitch Machine; Vocoder Lead; Yah Stab; Vowel Pluck; Synth Kick; Clap Room""",
    "House / Techno": """Deep House Organ Bass; Garage Organ Bass; House Pluck Bass; Acid Line; Squelch 303; Pluck Bass;
        Moog Thump; Bit Bass; CZ Resonance Bass; Detuned Square Bass; M1 House Piano; House Organ Stab; Stab Chord;
        Soft Stab Keys; Chord Memory; Organ Pad; Acid Arp; Pulse Train; Seq Bubbles; Morph Steps; Random Blips; Dark Pulse Seq;
        Acid Lead; Wah Lead; Modulo Lead; Buchla Bounce; Plaits Lead; Folded Lead; Bubble Pluck; Rubber Pluck;
        Funky Wah Pluck; Deep Pluck; Metallic Ring; Steel Drum; Cowbell 80s; Kaoss Kit Hit; Metal Hit; Glitch Hit; Synth Tom;
        Tom Electro; Synth Kick; Closed Hat; Clap Room; Rim Click; Plaits Thump; Dark Drone; Robot Chatter; Radio Static;
        Sidechain Stabs; Robot Voice; Eeh Choir""",
    "Pop": """Bright Saw Lead; Square Hero; Silk Lead; Vocal Synth Lead; Future Bass Saw; Chord Stab; Lush Pluck; Glass Pluck;
        Bell Pluck; Chime Pluck; Digital Bells; Marimba Pluck; Mallet Pluck; Ukulele; String Pluck; Water Drop; Tropical Pluck;
        Island Pluck; Poly Synth Keys; Digital Grand; Piano Air; Plastic Keys; Glass Keys; Juno Keys; Clavinet Pop;
        Crystal Bell; Glass Chimes; Bright Poly Pad; Warm Analog Pad; Silk Pad; Shimmer Pad; String Ensemble; Spiccato;
        Brass Fanfare; Synth Brass; Jump Brass; Aah Choir; Ooh Choir; Vowel Pluck; Yah Stab; Fat Unison Bass; Pluck Bass;
        Rubber Sub; Detuned Square Bass""",
    "Synthwave / Retro": """80s Poly Pad; Prophet Vector; PPG Sweep; Retro Sci-Fi Pad; Analog Strings Pad; PWM Strings;
        Pulse Width Pad; Phaser Strings; Analog Warmth; Warm Analog Pad; Glide Saw 80s; Analog Brass Lead; Chip Lead;
        Chip Arp; Square Hero; OP Lead; Hoover Lead; Digi Piano 84; Juno Keys; OP Keys; Vector Keys; Poly Synth Keys;
        Phase Keys; Prophet Brass; Synth Brass; Big Synth Horns; Analog String Machine; Ensemble 78; Solina Dream;
        Retro Arcade; Saw Sequence; Pulse Train; Cowbell 80s; Tom Electro; Synth Tom; Bit Bass;
        CZ Resonance Bass; Saw Stack Bass; Moog Thump; Dial Up; Laser Zap; Robot Voice; Vocoder Lead; Choir of Glass""",
    "Ambient / Cinematic": """Aurora; Cloud Nine; Daydream Swell; Dream Piano; Ethereal Voices; Floating; Hazy Memories;
        Hologram; Lullaby; Lush Supersaw Pad; Moonlit Bells; Northern Lights; Shimmer Strings; Snowfall; Soft Focus; Stardust;
        Starlight Keys; Sunset Glow; Tide Pool; Underwater Dream; Velvet Haze; Weightless; Cathedral; Choir of Glass;
        Comb Shimmer; Dark Matter; Desert Wind Pad; Evolving Spectra; Glacier; Glass Choir Pad; Granular Haze; Granular Pad;
        Harmonic Drift; Ice Field; Spectral Smear; Sunrise Sweep; Vowel Pad; Additive Choir; Breathing Pad; Night Pad;
        Phase Pad; Dark Choir; Ghost Strings; Night Swell; Void Organ; Haze Vox Pad; Swell Brass Pad; Soft French Horn;
        Tuba Low; Cinematic Swell; Cello Bow; Chamber Strings; Low Strings; Tremolo Strings; Viola Warm; Violin Solo;
        Glass Strings; Gamelan; Bowl Resonance; Wind Chime; Tubular Chime; FM Bell Pad; Celeste; Glass Harp; Glass Organ;
        Hammer Dulcimer; Harpsichord; Harp Cascade; Gated Choir; Space Bloom; Space Drone; Dark Drone; Wind Tunnel;
        Underwater Sonar; Insect Swarm; Alarm Siren; Monk Chant; Choir Swell; Whisper Pad; Ghost Vowels; Formant Sweep Voice;
        Siren Voice; Zither; Shamisen; Koto Pluck; Tabla Bend""",
    "Lo-Fi / Chill": """Lo-Fi Keys; Rhodes Lofi Tape; Rhodes Hip Hop; Rhodes Mellow Jazz; Rhodes Suitcase; Rhodes Echo Room;
        Tape Soul Rhodes; Late Night Rhodes; Upright Felt; Analog Piano; Toy Keys; Toy Piano; Music Box; Wurly Drive;
        Kalimba Drop; Water Drop; Marimba Wood; Vibraphone Soft; Vibes Tremolo; Nylon Pluck; Guitar Pick; Muted Guitar;
        Harp Glint; Warm Blanket; Soft Focus; Hazy Memories; Velvet Haze; Sunset Glow; Dream Piano; Lullaby; Tide Pool;
        Smooth Sub; Liquid Sub; Soft Sine Lead; Ocarina; Flute Synth; Nasal Reed; Chebyshev Whistle; Radio Static; Tape Stop;
        Conga Tone; Rim Click; Seq Bubbles; Satin Pad""",
}


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Content", "Patches")
    files = {}   # short name -> file name
    for folder in sorted(os.listdir(root)):
        for f in sorted(os.listdir(os.path.join(root, folder))):
            if f.endswith(".vital"):
                files[f[:-6].split(" ", 1)[1]] = f
    tags = {f: [] for f in files.values()}
    bad = []
    for genre, names in TAGS.items():
        assert genre in GENRES, genre
        for n in (x.strip() for x in names.split(";")):
            if not n:
                continue
            if n not in files:
                bad.append(f"{genre}: unknown patch '{n}'")
                continue
            if genre not in tags[files[n]]:
                tags[files[n]].append(genre)
    for f, g in tags.items():
        if not g:
            bad.append(f"no genre: {f}")
    if bad:
        print("\n".join(bad))
        sys.exit(1)
    out = os.path.join(root, "..", "Genres.txt")
    with open(out, "w", newline="\n") as fh:
        fh.write("# Warpline factory genres: <patch file>|<genre>,<genre>   (made by tools/genres.py)\n")
        for f in sorted(tags):
            g = sorted(tags[f], key=GENRES.index)
            fh.write(f"{f}|{','.join(g)}\n")
    counts = {g: sum(g in v for v in tags.values()) for g in GENRES}
    print(f"{len(tags)} patches -> {out}")
    for g in GENRES:
        print(f"  {g}: {counts[g]}")


if __name__ == "__main__":
    main()
