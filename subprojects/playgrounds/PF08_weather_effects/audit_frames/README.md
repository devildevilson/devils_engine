# PF08 closing-audit frames

These eight PNGs are the permanent 1280x720, frame-80, no-overlay closing gallery. They were authored once
on 2026-08-30. `verify_audit_frames.sh` launches only PF08 and compares a temporary PPM against these files;
it never launches PF07 and never rewrites the references.

| Frame | Authored state | SHA-256 |
|---|---|---|
| `clear_noon.png` | `--preset=noon --weather=clear` | `14dcb34b498e9208db508a4ff2e03dfd6211fdb96d8c51d06536557966baddcb` |
| `clear_sunset.png` | `--preset=double_sunset --weather=clear` | `bb4292a7f366c478ad1b975e56d8482134212428b354517e9270d518b6b97d28` |
| `clear_night.png` | `--preset=night --weather=clear` | `6d1a1a9cddd50fdfce99ec40ab2fcd5763caa1a316d63b02e6b1e4104fed6cbb` |
| `overcast_noon.png` | `--preset=noon --weather=overcast` | `4767853e0b3821a7d7f897b90b40801da0ecb1a5864994c02d70ec61bb65d59d` |
| `rain_sunset.png` | `--preset=double_sunset --weather=rain --surface-age=1` | `3c2b40af55a03ed1921280ffaf44918373e1af54ba687b018f947c139cd7c456` |
| `snow_sunset.png` | `--preset=snow_glint --weather=snow --surface-age=30` | `68db465c509b1c7265c7a1cc4551889c4f25a54217d57bde38f61825a089a92e` |
| `lightning_magic.png` | `--lightning=magic --lightning-phase=0.03` | `5e180333767ccfd745984a5c171fa4067e864f9639d8cd8aa0520866b05016bb` |
| `aurora_night.png` | `--preset=aurora --weather=clear` | `1c0ab6e2ad32a3fb7ab62b83f4f47f53580d31691c99e531d54b0f3341fa1bc8` |

Regenerate these assets only after an explicit visual-baseline decision. Ordinary regression checks use:

```sh
bash subprojects/playgrounds/PF08_weather_effects/verify_audit_frames.sh
```
