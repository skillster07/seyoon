import { ShortsTemplate } from '../types';

export const seaMonsterShortsTemplate: ShortsTemplate = {
  project: {
    title: "국가별 바다괴물",
    target_platform: ["TikTok", "Reels", "Shorts"],
    video_spec: {
      duration_s: 59.653,
      fps: 60,
      resolution_px: { w: 2160, h: 3840 },
      aspect_ratio: "9:16",
      delivery_codec_hint: "h264_or_h265",
      audio_hint: { codec: "aac", sample_rate_hz: 44100, channels: 2 }
    }
  },
  global_style: {
    visual_tone: [
      "cinematic",
      "dark-fantasy maritime",
      "high contrast",
      "volumetric fog/mist",
      "wet surfaces, sea spray",
      "subtle film grain",
      "realistic lighting"
    ],
    color_grade: {
      dominant: ["deep teal", "steel blue", "storm gray"],
      accent_by_scene_allowed: true,
      accent_examples: ["golden hour warm highlights (Greece)"]
    },
    camera_language: [
      "establishing wide shots over ocean",
      "ship-deck handheld tension shots",
      "monster reveal with scale emphasis",
      "occasional top-down deck shot",
      "slow push-in + whip-cut transitions"
    ],
    vfx: [
      "ocean simulation feel",
      "spray particles",
      "motion blur on fast action",
      "lightning/overcast sky where relevant"
    ],
    negative_prompt: [
      "watermark",
      "logo",
      "extra random text",
      "gibberish Korean",
      "broken flag shapes",
      "deformed hands/faces",
      "duplicated limbs",
      "low-res artifacts",
      "cartoonish shading (unless style_override is set)"
    ]
  },
  overlay_system: {
    country_tag: {
      position: "bottom_center",
      layout: "flag_icon_above_country_name",
      country_name_language: "ko",
      country_name_style: {
        font_family_hint: "bold_sans",
        font_color: "white",
        shadow: "soft_drop_shadow",
        outline: "none_or_very_subtle",
        size_ratio_of_frame_height: 0.035
      },
      flag_style: {
        shape: "rect",
        size_ratio_of_frame_height: 0.03,
        margin_px: 8
      },
      safe_area_margin_px: 160
    },
    subtitle: {
      position: "center_lower_third",
      font_family_hint: "bold_sans",
      font_color: "white",
      shadow: "soft_drop_shadow",
      max_chars_per_line: 12,
      timing_mode: "per_shot_or_per_phrase"
    },
    title_card: {
      enabled: true,
      text: "국가별 바다괴물",
      position: "top_center",
      style: {
        font_family_hint: "brush_calligraphy",
        fill_color: "yellow",
        stroke: "dark_outline",
        size_ratio_of_frame_height: 0.06
      },
      show_in_scene_id: "SC01_JP"
    }
  },
  audio_plan: {
    music: {
      genre: "cinematic trailer / dark ambient",
      energy_curve: [
        { t: 0, level: 0.6 },
        { t: 10, level: 0.7 },
        { t: 30, level: 0.85 },
        { t: 52, level: 0.95 },
        { t: 59.6, level: 0.3 }
      ]
    },
    sfx: ["storm_wind", "waves_crash", "ship_creak", "monster_rumble"],
    voiceover: {
      language: "ko",
      tone: "serious narrator",
      pace_wpm_hint: 150,
      script_mode: "optional",
      script_placeholder_note: "원본 내레이션을 그대로 복제하려면 대사/내레이션 전문이 필요. 여기서는 장면별 한 줄 훅으로 대체."
    }
  },
  scenes: [
    {
      id: "SC01_JP",
      time_range_s: { start: 0.0, end: 10.0 },
      country: { name_ko: "일본", flag: "JP" },
      theme_monster: "우미보즈(거대한 검은 해괴) 느낌",
      shots: [
        {
          shot_id: "JP_A",
          duration_s: 3.5,
          prompt: "stormy night ocean, old wooden ships battling huge waves, cinematic wide establishing shot, mist and spray, ominous atmosphere, no modern objects",
          camera: "slow aerial drift forward, slight roll from waves"
        },
        {
          shot_id: "JP_B",
          duration_s: 3.0,
          prompt: "top-down view on wet ship deck, sailor scrambling/crawling, rain droplets, lantern glow, tension",
          camera: "top-down static with subtle shake"
        },
        {
          shot_id: "JP_C",
          duration_s: 3.5,
          prompt: "massive black humanoid sea entity rising behind the ship (umibozu-like), water pouring off its head, scale shock, ship in foreground",
          camera: "medium-wide reveal, slow push-in, emphasize size"
        }
      ],
      subtitles: [
        { t: 8.8, text: "…" }
      ]
    },
    {
      id: "SC02_GR",
      time_range_s: { start: 10.0, end: 20.0 },
      country: { name_ko: "그리스", flag: "GR" },
      theme_monster: "세이렌/인어(매혹적이지만 위험)",
      shots: [
        {
          shot_id: "GR_A",
          duration_s: 3.5,
          prompt: "golden hour sea, ancient Greek galley sailing near a towering rock spire, calm but ominous, cinematic aerial",
          camera: "high-angle tracking along the ship"
        },
        {
          shot_id: "GR_B",
          duration_s: 3.5,
          prompt: "realistic mermaids sitting on wet rocks at sunset, elegant draped garments, ocean foam, mythic mood",
          camera: "low angle, slow pan across figures"
        },
        {
          shot_id: "GR_C",
          duration_s: 3.0,
          prompt: "mermaids diving into crashing waves beside the ship, splash and foam, sense of lure and danger",
          camera: "close-to-water perspective, quick cut"
        }
      ],
      subtitles: []
    },
    {
      id: "SC03_NO",
      time_range_s: { start: 20.0, end: 30.0 },
      country: { name_ko: "노르웨이", flag: "NO" },
      theme_monster: "크라켄(촉수/흡반 클로즈업)",
      shots: [
        {
          shot_id: "NO_A",
          duration_s: 3.5,
          prompt: "Viking longships in dark stormy sea, cold blue grade, heavy clouds, rough waves",
          camera: "wide shot with slow lateral drift"
        },
        {
          shot_id: "NO_B",
          duration_s: 3.0,
          prompt: "extreme close-up of massive tentacle with suction cups sliding underwater, bubbles, dark depth",
          camera: "macro-like close-up, slow glide"
        },
        {
          shot_id: "NO_C",
          duration_s: 3.5,
          prompt: "chaos on wooden deck, crew slipping, rain and spray, tentacle threat implied off-frame",
          camera: "handheld shaky medium shot"
        }
      ],
      subtitles: []
    },
    {
      id: "SC04_ES",
      time_range_s: { start: 30.0, end: 40.0 },
      country: { name_ko: "스페인", flag: "ES" },
      theme_monster: "거대 해양괴(소용돌이/선체 옆 출현)",
      shots: [
        {
          shot_id: "ES_A",
          duration_s: 3.5,
          prompt: "Spanish galleon in dusk storm, strange circular swell/whirlpool forming in ocean ahead, ominous calm in center",
          camera: "high wide shot toward horizon"
        },
        {
          shot_id: "ES_B",
          duration_s: 3.0,
          prompt: "sailors on deck in fear, lantern glow, rain, desperate expressions",
          camera: "tight handheld close shot"
        },
        {
          shot_id: "ES_C",
          duration_s: 3.5,
          prompt: "gigantic sea creature surfacing alongside the ship, textured wet skin, scale comparison, spray",
          camera: "side reveal, slow push-in"
        }
      ],
      subtitles: [
        { t: 32.0, text: "기도해!" }
      ]
    },
    {
      id: "SC05_NZ",
      time_range_s: { start: 40.0, end: 51.5 },
      country: { name_ko: "뉴질랜드", flag: "NZ" },
      theme_monster: "거대 등가시 해룡(바다 위로 등판)",
      shots: [
        {
          shot_id: "NZ_A",
          duration_s: 3.5,
          prompt: "open ocean under overcast sky, small outrigger-style boats/sails, distant swell hinting something massive below",
          camera: "aerial wide, slow forward"
        },
        {
          shot_id: "NZ_B",
          duration_s: 4.0,
          prompt: "enormous spined back emerging from the sea, mist and spray, boats dwarfed by monster",
          camera: "wide reveal, hold for scale"
        },
        {
          shot_id: "NZ_C",
          duration_s: 4.0,
          prompt: "close-up of monster head/skin surface near the boat, water streaming, tense proximity",
          camera: "close shot from boat perspective"
        }
      ],
      subtitles: []
    },
    {
      id: "SC06_KR",
      time_range_s: { start: 51.5, end: 59.653 },
      country: { name_ko: "한국", flag: "KR" },
      theme_monster: "해룡/이무기 계열(안개 속 선단, 거대 몸통/꼬리)",
      shots: [
        {
          shot_id: "KR_A",
          duration_s: 3.0,
          prompt: "stormy ship deck POV, crew spotting a colossal form in fog, wet wood, cold blue grade",
          camera: "POV medium shot, quick push"
        },
        {
          shot_id: "KR_B",
          duration_s: 2.5,
          prompt: "interior/deck chaos as wave slams the ship, debris and water, panic",
          camera: "handheld shake, fast cut"
        },
        {
          shot_id: "KR_C",
          duration_s: 2.6,
          prompt: "wide shot: multiple ships in misty ocean, huge serpentine body/tail slicing through water beside them, epic scale",
          camera: "high wide, slow drift back"
        }
      ],
      subtitles: [
        { t: 52.0, text: "저기다!" }
      ]
    }
  ],
  remix_knobs: {
    what_to_change_for_new_version: [
      "countries_list",
      "monster_archetypes_per_country",
      "era_of_ships (ancient / medieval / modern navy / sci-fi)",
      "color_grade accents per scene",
      "subtitle hooks (짧은 한 줄 공포/전설 요약)",
      "music genre (epic / horror / tribal / synthwave)"
    ],
    countries_list_example_new: [
      { name_ko: "멕시코", flag: "MX", monster: "심해 해골고래" },
      { name_ko: "이집트", flag: "EG", monster: "나일-바다 접합 괴룡" },
      { name_ko: "인도", flag: "IN", monster: "거대 나가(해룡)" },
      { name_ko: "캐나다", flag: "CA", monster: "빙하 심해 괴수" },
      { name_ko: "인도네시아", flag: "ID", monster: "화산해 협곡 크라켄" },
      { name_ko: "프랑스", flag: "FR", monster: "안개 속 유령선+해마귀" }
    ],
    format_lock: {
      keep_duration: true,
      keep_overlay_layout: true,
      keep_cut_pace: true,
      keep_realistic_cinematic_style: true
    }
  }
};

// Helper function to get flag emoji from country code
export function getFlagEmoji(countryCode: string): string {
  const codePoints = countryCode
    .toUpperCase()
    .split('')
    .map(char => 127397 + char.charCodeAt(0));
  return String.fromCodePoint(...codePoints);
}

// Template registry for future expansion
export const templateRegistry: Record<string, ShortsTemplate> = {
  'sea-monster-shorts': seaMonsterShortsTemplate,
};

export default seaMonsterShortsTemplate;
