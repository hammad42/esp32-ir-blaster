# MQTT & Home Assistant

Enable it under **Settings → MQTT & Home Assistant**: broker host, port,
credentials, and a base topic (default `irblaster`).

---

## Topics

With the default base topic:

| Topic | Direction | Payload |
|---|---|---|
| `irblaster/status` | device → | `online` / `offline`, retained. This is also the last-will, so the broker publishes `offline` if the device drops off without saying goodbye. |
| `irblaster/send` | → device | a command name or id, or a JSON object |
| `irblaster/raw` | → device | `{"raw":[...],"khz":38,"repeats":1}` |
| `irblaster/learn` | → device | `start` or `cancel` |
| `irblaster/state` | device → | JSON status, retained, every 30 s |
| `irblaster/event` | device → | JSON, published on each transmission |

### Sending

```bash
mosquitto_pub -h 192.168.1.10 -t irblaster/send -m 'AC_Power'
mosquitto_pub -h 192.168.1.10 -t irblaster/send -m '{"cmd":"TV_Volume_Up","repeats":3}'
```

Both the friendly name (case-insensitive) and the 8-character id work.

### State

```json
{"ip":"192.168.1.42","rssi":-52,"uptime":86400,"heap":148320,
 "commands":37,"last":"AC_Cool_24","tx":1841,"fw":"1.0.0"}
```

### Events

```json
{"event":"sent","name":"AC_Power","id":"0000001a"}
{"event":"error","name":"unknown command","id":"AC_Powr"}
```

---

## Home Assistant

Leave **Publish Home Assistant discovery** on and every stored command appears
automatically as a `button` entity, tied to one device card named after your
hostname, with availability wired to the status topic.

Discovery is republished when you learn, rename or delete a command, and on
every broker reconnect. There is also a **Republish discovery** button on the
Settings tab for when Home Assistant has been reinstalled.

Entities are named after your commands, so `AC_Cool_24` becomes
`button.ir_blaster_ac_cool_24`. This is the strongest argument for naming
commands carefully the first time.

### Discovery payloads are large — check your broker

Each config message is ~350–450 bytes and they are retained. A hundred commands
is ~40 KB of retained data, published four at a time so a broker's receive
window is never overrun. Mosquitto handles this without configuration.

### A climate entity from learned states

Home Assistant's `climate` template can drive an A/C from the discrete states
you learned:

```yaml
climate:
  - platform: climate_template
    name: Living Room AC
    modes: [ "off", "cool", "heat" ]
    min_temp: 22
    max_temp: 26
    temp_step: 2

    set_hvac_mode:
      - choose:
          - conditions: "{{ hvac_mode == 'off' }}"
            sequence:
              - service: button.press
                target: { entity_id: button.ir_blaster_ac_off }
          - conditions: "{{ hvac_mode == 'cool' }}"
            sequence:
              - service: button.press
                target: { entity_id: button.ir_blaster_ac_cool_24_auto }

    set_temperature:
      - service: button.press
        target:
          entity_id: >
            button.ir_blaster_ac_cool_{{ temperature | int }}_auto
```

Learn one command per temperature you actually use — `AC_Cool_22_Auto`,
`AC_Cool_24_Auto`, `AC_Cool_26_Auto` — and the template addresses them by name.

### Scripts without discovery

If you prefer explicit configuration:

```yaml
script:
  tv_on:
    sequence:
      - service: mqtt.publish
        data:
          topic: irblaster/send
          payload: "TV_Power"
```

### An availability-aware sensor

```yaml
mqtt:
  sensor:
    - name: "IR Blaster last command"
      state_topic: "irblaster/state"
      value_template: "{{ value_json.last }}"
      json_attributes_topic: "irblaster/state"
      availability_topic: "irblaster/status"
```

---

## Node-RED

An `mqtt out` node publishing to `irblaster/send` with the command name as the
payload is the whole integration.

To react to transmissions, subscribe to `irblaster/event` and parse the JSON —
useful for confirming a scene actually fired, since `event` is published only
after the transmitter reports success.

---

## Notes

**Sends block the device briefly.** A long A/C burst with repeats occupies the
loop for a second or two. The MQTT keepalive is 30 s, so this is comfortable,
but do not fire a dozen commands into the topic at once and expect them all to
land — space them by a second.

**Reconnection is backed off.** Failures double the retry interval from 5 s up
to a 60 s cap. A broker that is down for a week does not become a reconnect
storm the moment it returns.

**Deleting a command clears its entity.** The firmware publishes an empty
retained payload to the discovery topic, which is how Home Assistant is told to
forget an entity. If you clear the store some other way (a factory reset), the
old entities linger until you delete the device from the MQTT integration page.

**Credentials are stored in plain NVS** and MQTT runs unencrypted. That is
normal for this class of device on a home LAN; do not point it at a broker
whose credentials matter elsewhere.
