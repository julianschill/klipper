Addressable LEDs are beginning to supercede RGB LEDs for their
flexibility and relative ease of use. With each individual element
capable of displaying an entire spectrum of colors as very high speed, 
they can be used to create a variety of lighting effects.

At this time, klipper supports most WS2812 compatible (neopixels)
and APA102 compatible (dotstar) chips for LED Effects. 

# Wiring WS2812 compatible (neopixel) LEDs

Neopixel type LEDs require one digital IO pin and a voltage supply. 
Most are 5V but can be driven from a 3V source. Check manufacturer 
specifications to ensure they will work with your board. Each individual
emitter has 4 pins. VCC, GND, Din, and Dout. Neopixel strips typically
have 3 solder pads or a connector with 3 pins and arrows indicating
the direction of the data. The D pins are unidirectional and cannot be
reversed. When attaching them to your printer, the Din or D→ connection
should be attached to an available digital IO pin on the MCU board.
The VCC connection is attached to a supply voltages that is compatible
with the LED strip. Neopixels will typically use 60mA of current per
emitter at full brightness so depending on the power capabilities of
your printer board, it is recommended that they be powered from a 
separate power source. It is important that a GND wire be run from
the neopixel chain back to the MCU board in addition to the GND to
the power source. This will ensure the board can communicate with
the strips. 

At the present time, Klipper only supports 18 discrete emitters per 
IO pin. It is possible to wire two strips to the same data pin and 
have them show the same colors. It is also possible to specify 
multiple LED chains on different IO pins in the LED Effects 
configuration settings.

# Wiring APA102 compatible (dotstar) LEDs

APA102 dotstar LEDs are similar to the neopixel LEDs with the exception
that dotstar uses one-way SPI for communication. This requires the 
addition of a clock signal for the emitters. Multiple strips should be
able to share the same clock pin but they each require their own
data line.

# Configuring the strips

In your config file, each strip or chain connected to an IO pin must
have a definition. Following the example in [config/example-extras.cfg](example-extras.cfg) 
each one's data pin, and, if applicable, clock pin, is defined along
with the number of LEDs in the chain.

```
[neopixel panel_ring]
pin:                     ar6 
chain_count:             16
```

# Configuring the effects

Effects are, in a more abstract sense, a _state_ that the strips 
exist in. Effects can be comprised of 1 led or 100. There can be
one effect layer or 10. It is all arbitrary.

## Basic definition

For our example printer, there is one neopixel ring with 16 leds
that is situated on the front panel, and a short segment of 
neopixel LEDs next to the hot end for illuminating the print.

There are also 5 dotstar LEDs located underneath the bed.
Pin numbers listed here are completely made-up.

```
[neopixel panel_ring]
pin:                     ar6 
chain_count:             16
 
[neopixel tool_lights] 
pin:                     ar15 
chain_count:             6
 
[neopixel bed_lights] 
data_pin:                ar21 
clock_pin                ar22 
chain_count:             5
```

We would like the ring to turn on a light shade of blue when the
printer comes online and we want the brightness to _breathe_ in and out.

```
[led_effect panel_idle]
autostart:              true
frame_rate:             24
blend_mod:              add
leds:                               
    neopixel:panel_ring                       
effects:
    breathing  .5 1 [(.5,.5,1)]
```

This has defined an effect called `panel_idle` that can be controlled
via the gcode command `SET_LED_EFFECT EFFECT=panel_idle`

## Defining LEDs 

the `leds:` section is a list of neopixel or dotstar strips that will
make up the effect. Both types can be used for the same effect. Each
strip is defined on a separate line and indented beneath the `leds:`
section.

```
leds:                               
    neopixel:panel_ring  
    neopixel:tool_lights
    dotstar:bed_lights
``` 

Additionally, one may decide to only have certain LEDs displaying the
effect. This is accomplished by providing the index of the LEDs to be
used after the strip name. The index can be a list or a range. If the
indices are omitted, the entire strip is used.

As well, if for some reason you needed to, the same strip can be used
twice in an effect with different emitters being specified.

```
leds:                               
    neopixel:tool_lights 
    neopixel:panel_ring  (1-7) 
    neopixel:panel_ring  (9-16)   
    dotstar:bed_lights   (1,3,5)
``` 


## Defining Effect Layers
Effects are generated as frames. Each frame contains the number of pixels
equal to the number of LEDs defined for the effect. So an effect with 22
LEDs specified would have 22 pixels per frame.

Each effect layer is generated as a frame. Each layer frame is blended with
the next to generate the effect. Blending is cumulative and how colors are
blended is defined by the blending mode of the top layer.
Each effect layer is listed on its own line and each has its own settings.

```
layers:            
   breathing  .5 screen [(0,.1,1), (0,1,.5), (0, 1,1), (0,.1,.5)]
   static     1  bottom [(1,.1,0), (1,.1,0), (1,.1,0), (1,1,0)]
```

The first setting is the type of effect to display. There are several to
choose from and each has it's own unique behavior.

### Static
A single color is displayed and it does not change. If a palette of multiple
colors is provided, colors will be evenly blended along the LEDs based on
difference in hue.

### Breathing
Colors fade in and out. If a palette of multiple colors is provided, it will 
cycle through those colors in the order they are in the palette.
The effect speed parameter controls how long it takes to "breathe" one time.

### Blink
LEDs are turned fully on and fully off based on the effect speed. If a palette 
of multiple colors is provided, it will cycle through those colors in order

### Strobe
LEDs are turned fully on and then faded out over time with a decay. If a palette 
of multiple colors is provided, it will cycle through those colors in order

### Twinkle
Random flashes of light with decay along a strip. If a palette is specified,
a random color is chosen from the palette.

### Gradient
Colors from the palette are cycled through the leds

### Comet 
A light moves through the LEDs with a decay trail. Direction can be controlled
by using a negative speed value. The palette colors determine the color of the
comet and the tail. Brightness values for the tail are ignored and only the hue
value is used.

### Chase
Similar to comet, but with multiple lights chasing each other.

### Heater
This effect becomes active when the specified heater is at its target temperature
+/- 5 degrees. It is effectively a solid color.

### Heating
Colors respond to changes in the temperature of the heater defined in the 
heater section of the effect. The target temperature is compared to the actual
temperature and the appropriate color is selected from the palette. If only
one color is specified in the palette, the brightness will be gradually increased
to that color until the target is met.

Once the target temperature is met, the layer is disabled and will only display
black.

### Cooling
Similar to heater heating, colors are selected from the palette based on how hot
the specified heater is from a "safe" temperature. The "safe" temperature is
defined by the layer speed parameter in the config. Color selection from the
palette is determined by the difference between the current temperature and the
"safe" temperature. When the heater has cooled and is safe to touch, the layer
is disabled.

### AnalogPin
This effect uses the value read from an analog pin to determine the color.
If multiple colors are specified in the palette, it chooses one based on the
value of the pin. If only one color is specified, the brightness is proportional
to the pin value. An example usage would be attaching an analog potentiometer
that controls the brightness of an LED strip.

## Blending Effect Layers
If you have ever used image editing software you may be familiar with
color blending between image layers. Several common color blending
techniques have been added to blend layers together. Layers defined
in the configuration are ordered top to bottom. If there are 3 layers
defined, the topmost layer is first blended with the middle layer, then
the result is blended with the bottom layer. The bottom layer will never
be blended with anything even if a blending mode is specified for it.

### bottom
No blending is done, the value from the color channel of the bottom layer is used.

### top
No blending is done, the value from the color channel of the top layer is used.

### add 
```
    ( a + b )
```
Color channels (Red, Green, and Blue) are added to one another. This results
in channels becoming brighter.

### subtract 
```
    ( a - b ) 
```
The the bottom layer is subtracted from the top layer. This results in darkening
similar colors.

### difference
```
    ( a - b ) or ( b - a )
```
The darker of the layers is subtracted from the brighter of the two

### average
```
    ( a + b) / 2
```
The average of the channels is taken

### multiply
```
    ( a * b )
```
The channels are multiplied together, this is useful to darken colors

### divide
```
    ( a / b )
```
The channels are divided, this results in brightening colors, often to white

### screen
```
    1 - ( 1 - a ) * ( 1 - b)
```
The values are inverted, multiplied, and then inverted again. Similar to
divide, it results in brighter colors

### lighten
```
    ( a if a > b else b )
```
The brigther of the color channels is used

### darken
```
    ( a if a < b else b )
```
The opposite of lighten, the darker of color channels is used 

### overlay
```
    ( 2ab if a > .5 else 1-2(1-a)(1-b) )
```
    Overlay is a combination of multiply and screen. This has a similar effect
    of increasing contrast.




# Sample Configurations

## das Blinkenlights
in the event of critical error, all LED strips breath red in unision to
provide a visible indicator of an error condition with the printer. This
effect is disabled during normal operation and only starts when the MCU
enters a shutdown state.

```
[led_effect critical_error]
leds:                               
    neopixel:bed_lights
    neopixel:tool_lights
    neopixel:panel_lights
run_on_error:                       true
autostart:                          false
frame_rate:                         24
layers:                             
    breathing 1 2 none [(1,0,0)]
```

## Bed Idle with Temperature
[led_effect bed_effects]
leds:                               
    neopixel:bed_lights
autostart:                          true
basecolor:                          1,0,0
frame_rate:                         24
heater:                             heater:bed
layers:
    heater  1 2 add  [(1,0,0)]
    cooling 1 2 add  [(1,1,0),(1,0,0)]                             
    heating 1 2 add  [(1,1,0),(1,0,0)]
    static  1 2 none [(1,0,0)]


## Disco Infero