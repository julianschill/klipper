# Support for addressable LED visual effects 
# using neopixel and dotstar LEDs
#
# Copyright (C) 2020  Paul McGowan <mental405@gmail.com>
#
# This file may be distributed under the terms of the GNU GPLv3 license.

import neopixel, dotstar
import logging
from math import cos, exp, pi
from random import randint

ANALOG_SAMPLE_TIME  = 0.001
ANALOG_SAMPLE_COUNT = 10
ANALOG_REPORT_TIME  = 0.1

######################################################################
# Custom color value list, returns lists of [r, g ,b] values
# from a one dimensional list
######################################################################

class colorArray(list):
    def __getitem__(self, a):
        if isinstance(a, int):
            return super(colorArray, self).__getitem__(
                            slice(a*3, a*3+3))
        if isinstance(a, slice):
            return colorArray(
                        super(colorArray, self).__getitem__(
                            slice(a.start*3, a.stop*3, a.step)))
    def __getslice__(self, a, b):
        return self.__getitem__(slice(a,b))
    def __setitem__(self, a, v):
        if isinstance(a, int):
            super(colorArray, self).__setitem__(a*3  , v[0])
            super(colorArray, self).__setitem__(a*3+1, v[1])
            super(colorArray, self).__setitem__(a*3+2, v[2])   
    def __len__(self):
        return super(colorArray, self).__len__() / 3
    def reverse(self):
        self[:] = [c for cl in range(len(self)-1,-1, -1) 
                        for c in self[cl]]
    def shift(self, shift=1, direction=1):
        if direction:
            #shift array to the right
            self[:] = self[-shift:] + self[:-shift]
        else:
            #shift array to the left
            self[:] = self[shift:] + self[:shift]
    def padLeft(self, v, a):
        self[:] = v * a + self[:]
    def padRight(self, v, a):
        self[:] = self[:] + v * a

######################################################################
# LED Effect handler
######################################################################

class ledEffect:
    def __init__(self, config):
        self.config       = config
        self.printer      = config.get_printer()
        self.gcode        = self.printer.lookup_object('gcode')

        self.iteration    = 0        
        self.repeat       = 0
        self.layers       = []

        self.name         = config.get_name().split()[1]
        self.frameRate    = 1.0 / config.getfloat('frame_rate', default=24, minval=1, maxval=60)
        self.autoStart    = config.getboolean('autostart', False)
        self.runOnShutown = config.getboolean('run_on_error', False)
        self.heater       = config.get('heater', None)
        self.analogPin    = config.get('analog_pin', None)
        self.stepper      = config.get('stepper', None)
        self.configLayers = config.get('layers')
        self.configLeds   = config.get('leds')

        self.blendingModes  = {'top'       : (lambda t, b: t ),
                               'bottom'    : (lambda t, b: b ),
                               'add'       : (lambda t, b: t + b ),
                               'subtract'  : (lambda t, b: (t - b) * (t - b > 0)),
                               'difference': (lambda t, b: (t - b) * (t > b) + (b - t) * (t <= b)),
                               'average'   : (lambda t, b: 0.5 * (a + b)),
                               'multiply'  : (lambda t, b: t * b),
                               'divide'    : (lambda t, b: t / b if b > 0 else 0 ),
                               'screen'    : (lambda t, b: 1.0 - (1.0-t)*(1.0-b) ),
                               'lighten'   : (lambda t, b: t * (t > b) +  b * (t <= b)),
                               'darken'    : (lambda t, b: t * (t < b) +  b * (t >= b)),
                               'overlay'   : (lambda t, b:  
                                                    2.0 * t * b if t > 0.5 else 1.0 - (2.0 * (1.0-t) * (1.0-b))) }

        if self.analogPin:
            ppins = self.printer.lookup_object('pins')
            mcu_adc = ppins.setup_pin('adc', self.analogPin)
            mcu_adc.setup_adc_callback(ANALOG_REPORT_TIME, self.adcCallback)
            mcu_adc.setup_minmax(ANALOG_SAMPLE_TIME, ANALOG_SAMPLE_COUNT)
            query_adc = self.printer.load_object(self.config, 'query_adc')
            query_adc.register_adc(self.name, mcu_adc)

        
        self.printer.register_event_handler('klippy:ready', self._handle_ready)
        self.gcode.register_mux_command('SET_LED_EFFECT', 'EFFECT', self.name,
                                         self.cmd_SET_LED_EFFECT,
                                         desc=self.cmd_SET_LED_help)

    cmd_SET_LED_help = 'Starts or Stops the specified led_effect'

    def _handle_ready(self):
        self.reactor = self.printer.get_reactor()

        chains = self.configLeds.split('\n')

        if self.stepper:
            self.toolhead = self.printer.lookup_object('toolhead')
            kin = self.toolhead.get_kinematics()

            for r in kin.rails:
                steppers = r.get_steppers() 

                for s in steppers:
                    if s.get_name() == self.stepper:
                        axis = s.get_name(short=True)
                        if axis == 'x': self.stepperAxis = 0
                        if axis == 'y': self.stepperAxis = 1
                        if axis == 'z': self.stepperAxis = 2                   
                        self.stepperRange = r.get_range()
                        self.stepperPosition = kin.calc_tag_position
                        self.stepperTimer = self.reactor.register_timer(self._pollStepper, 
                                                self.reactor.NOW) 

        if self.heater:             
            pheater = self.printer.lookup_object('heaters')
            self.heater = pheater.lookup_heater(self.heater)
            self.heaterTimer = self.reactor.register_timer(self._pollHeater, self.reactor.NOW) 

        self.ledChains    = []
        self.leds         = []

        #map each LED from the chains to the "pixels" in the effect frame
        for chain in chains:
            chain = chain.strip()
            parms = [parameter.strip() for parameter in chain.split(' ')
                        if parameter.strip()]

            if parms:
                ledChain     = self.printer.lookup_object(parms[0].replace(':',' '))
                ledIndices   = ''.join(parms[1:]).strip('()').split(',')

                #Add a call for each chain that orders the colors correctly
                if hasattr(ledChain, 'color_order_GRB') and ledChain.color_order_GRB: 
                    getColorData = (lambda r, g, b:             
                                    ( int(self._clamp(g) * 254.0), 
                                      int(self._clamp(r) * 254.0), 
                                      int(self._clamp(b) * 254.0)))                
                else:         
                    getColorData = (lambda r, g, b:        
                                    ( int(self._clamp(r) * 254.0), 
                                      int(self._clamp(g) * 254.0), 
                                      int(self._clamp(b) * 254.0)))

                #Add each discrete chain to the collection
                if ledChain not in self.ledChains:
                    self.ledChains.append(ledChain)

                for led in ledIndices:
                    if led:
                        if '-' in led:
                            start, stop = map(int,led.split('-'))
                            for i in range(start-1, stop-1):
                                self.leds.append([ledChain, int(i) * 3, getColorData])
                        else:
                            for i in led.split(','):
                                self.leds.append([ledChain,(int(i)-1) * 3, getColorData])
                    else:
                        for i in range(ledChain.chain_count):
                            self.leds.append([ledChain, int(i) * 3, getColorData])
  
        self.ledCount = len(self.leds)

        #enumerate all effects from the subclasses of _layerBase...
        availableLayers   = {str(c).rpartition('.layer')[2].replace("'>", "").lower() : c
                                   for c in self._layerBase.__subclasses__()
                                   if str(c).startswith("<class")}  

        st = (lambda x : x.strip(('( )')))

        for layer in [line for line in self.configLayers.split('\n') if line.strip()]: 

            parms = [st(parameter) for parameter in layer.split(' ') if st(parameter)] 

            if not parms[0] in availableLayers:
                raise self.printer.config_error("LED Effect '%s' in section '%s' is not a valid effect layer" % (
                                parms[0], self.name))

            if not parms[3] in self.blendingModes:
                raise self.printer.config_error("Blending mode '%s' in section '%s' is not a valid blending mode" % (
                                parms[3], self.name))

            layer = availableLayers[parms[0]]

            palette = [float(st(c)) for t in parms[4:] for c in t.split(',') if st(c)]

            self.layers.insert(0, layer(effectRate    = float(parms[1]),  
                                        effectCutoff  = float(parms[2]),  
                                        paletteColors = palette,    
                                        frameRate     = self.frameRate,
                                        ledCount      = len(self.leds),
                                        blendingMode  = parms[3]))

        if self.autoStart:
            self.repeat = 1
            t = self.reactor.NOW
        else:
            t = self.reactor.NEVER

        self.frameTimer = self.reactor.register_timer(self._getFrames, t)     
        
        #TODO Run While Idle
        #self.printer.register_event_handler('#idle_timeout:idle', self._handle_shutdown)
        self.printer.register_event_handler('klippy:shutdown', self._handle_shutdown)

    def cmd_SET_LED_EFFECT(self, gcmd):
        if gcmd.get_int('STOP', 0) == 1:
            self.repeat = 0
            self.reactor.update_timer(self.frameTimer, self.reactor.NEVER)
        else:            
            self.repeat = 1
            self.reactor.update_timer(self.frameTimer, self.reactor.NOW)

    def _handle_shutdown(self):

        for chain in self.ledChains:
            chain.color_data = [] * (chain.chain_count * 3)
            chain.send_data()

        if self.runOnShutown:
            if self.frameTimer:
                self.reactor.update_timer(self.frameTimer, self.reactor.NOW)            
            else:
                self.frameTimer = self.reactor.register_timer(self._getFrames, self.reactor.NOW)    
        else:
            if self.frameTimer:
                self.reactor.unregister_timer(self.frameTimer)

    def _pollHeater(self, eventtime):
        current, target = self.heater.get_temp(eventtime)
        
        for layer in self.layers:
            layer.heaterCurrent = current
            layer.heaterTarget  = target
            if target > 0:
                layer.heaterLast = target

        return eventtime + 1

    def _pollStepper(self, eventtime):
        p = self.stepperPosition()[self.stepperAxis]
        if p >= self.stepperRange[0] and p <= self.stepperRange[1]:
            r = int((self._clamp((p / (self.stepperRange[1] - self.stepperRange[0]))) * 100) - 1)
            for layer in self.layers:
                layer.stepperPosition = r
        return eventtime + .5

    def adcCallback(self, read_time, read_value):
        v = int(read_value * 1000) / 10.0
        for layer in self.layers:
            layer.analogValue = v

    def _clamp(self, val):
        if val < 0.0: return 0.0
        if val > 1.0: return 1.0
        return val

    def _getFrames(self, eventtime):
        frame = [0.0] * 3 * self.ledCount
     
        for layer in self.layers:
            layerFrame = layer.nextFrame(eventtime)
            if layerFrame:
                blend = self.blendingModes[layer.blendingMode]
                frame = [blend(t, b) for t, b in zip(layerFrame, frame)]
            
        for i in range(self.ledCount):
            s = self.leds[i][1]
            chain =  self.leds[i][0]
            getColorData =  self.leds[i][2] 
            chain.color_data[s:s+3] = getColorData(*frame[i*3:i*3+3])

        for chain in self.ledChains:
            chain.send_data()
        if self.repeat > 0:
            return eventtime + self.frameRate
        else:
            for chain in self.ledChains:
                chain.color_data = [] * (chain.chain_count * 3)
                chain.send_data()
            return self.reactor.NEVER

    ######################################################################
    # LED Effect layers
    ######################################################################

    # super class for effect animations. new animations should
    # inherit this and return 1 frame of [r, g, b] * <number of leds> 
    # per call of nextFrame()
    class _layerBase(object):
        def __init__(self, **kwargs):
            self.ledCount        = kwargs['ledCount']
            self.paletteColors   = colorArray(kwargs['paletteColors'])
            self.effectRate      = kwargs['effectRate']
            self.effectCutoff    = kwargs['effectCutoff']
            self.frameRate       = kwargs['frameRate']
            self.blendingMode    = kwargs['blendingMode']
            self.frameNumber     = 0
            self.thisFrame       = []
            self.frameCount      = 1
            self.heaterCurrent   = 0
            self.heaterTarget    = 0
            self.heaterLast      = 100
            self.analogValue     = 0
            self.lastAnalog      = 0
            self.stepperPosition = 0

            if self.effectRate > 0:
                self.direction = 1
            else:
                self.direction = 0
                self.effectRate *= -1

        def nextFrame(self, eventtime):
            self.frameNumber += 1
            self.frameNumber = self.frameNumber*(self.frameNumber<self.frameCount)
            self.lastFrameTime = eventtime

            return self.thisFrame[self.frameNumber]

        def _decayTable(self, factor=1, rate=1):

            frame = [] 

            p = (1.0 / self.frameRate)
            r = (p/15.0)*factor

            for s in range(0, int((rate<1)+rate)):
                frame.append(1.0)
                for x in range(2, int(p / rate)+3):
                    b = exp(1)**-(x/r)
                    frame.append(b*(b>.01))

            return frame
           
        def _gradient(self, palette, steps, reverse=False):
            #fill the number of steps with an even number of divisions
            palette = colorArray(palette[:])

            if len(palette) == 1:
                return colorArray(palette * steps)
            else:
                divs = int(steps / (len(palette)-1)) + 1

            if reverse: palette.reverse()

            thisColor = palette[0]
            gradient  = palette[0]

            for i in range(1, len(palette)):
                nextColor = palette[i]        
                for t in range(1, divs):
                    z = [thisColor[j] + 
                                (float(t)/(divs-1))*(nextColor[j]-thisColor[j])
                                for j in range(3)]
                    gradient += z
                thisColor = nextColor

            return gradient

    #Solid color
    class layerStatic(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerStatic, self).__init__(**kwargs)

            self.paletteColors = colorArray(self.paletteColors)
            
            gradientLength = (3 - int(self.ledCount) % 3) + int(self.ledCount)
            gradient   = colorArray(self._gradient(self.paletteColors, gradientLength))
            
            self.thisFrame.append(gradient[0:self.ledCount])   
            self.frameCount = len(self.thisFrame)

    #Slow pulsing of color
    class layerBreathing(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerBreathing, self).__init__(**kwargs)

            brightness = []

            p = (1.0 / self.frameRate) * (self.effectRate * 0.5)
            o = int(p) 
            f = 2 * pi
        
            for x in range(0, int(p)):
                if x < p:
                    v  = (exp(-cos((f / p) * (x+o)))-0.367879) / 2.35040238
                else:
                    v = 0

                #clamp values
                if v > 1.0:
                    v = 1.0
                elif v < 0.0:
                    v = 0.0
                
                brightness.append(v)

            for c in range(0, len(self.paletteColors)):  
                color = self.paletteColors[c]  
                  
                for b in brightness:                   
                    self.thisFrame += [[b * i for i in color] * self.ledCount]

            self.frameCount = len(self.thisFrame)
            
    #Turns the entire strip on and off
    class layerBlink(_layerBase):    
        def __init__(self, **kwargs):
            super(ledEffect.layerBlink, self).__init__(**kwargs)

            frameCount = int(( 1.0 / self.frameRate ) * self.effectRate)

            for c in range(0, len(self.paletteColors)):  
                color = self.paletteColors[c]  
                self.thisFrame += [color * self.ledCount] * frameCount
                self.thisFrame += [[0,0,0] * self.ledCount] * frameCount

            self.frameCount = len(self.thisFrame)

    #Random flashes with decay
    class layerTwinkle(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerTwinkle, self).__init__(**kwargs)

            self.thisFrame = colorArray([0.0, 0.0, 0.0] * self.ledCount)
            self.lastBrightness  = [-1] * self.ledCount
            self.decayTable = self._decayTable(factor=1 / self.effectCutoff)
            self.decayLen = len(self.decayTable)
            self.colorCount = len(self.paletteColors) - 1

        def nextFrame(self, eventtime):

            for i in range(0, self.ledCount):
                
                r = randint(0, self.colorCount)
                color = self.paletteColors[r]       

                if randint(0, 255) > 254 - self.effectRate:
                    self.lastBrightness[i] = 0
                    self.thisFrame[i] = color

                if self.lastBrightness[i] != -1:
                    if self.lastBrightness[i] == self.decayLen:
                        self.lastBrightness[i] = -1
                        self.thisFrame[i] = [0.0, 0.0, 0.0]
                    else:
                        x = self.lastBrightness[i] 
                        self.lastBrightness[i] += 1
                        self.thisFrame[i] = [self.decayTable[x] * l 
                                                for l in self.thisFrame[i]]

            return self.thisFrame      
 
    #Blinking with decay
    class layerStrobe(_layerBase):
        def __init__(self, **kwargs):
            super(ledEffect.layerStrobe, self).__init__(**kwargs)

            decayTable = self._decayTable(factor=1 / self.effectCutoff,
                                          rate=self.effectRate)
            frameCount = int(( 1.0 / self.frameRate ) * self.effectRate)

            for c in range(0, len(self.paletteColors)):  
                color = self.paletteColors[c]  
                  
                for b in decayTable:                   
                    self.thisFrame += [[b * i for i in color] * self.ledCount]

            self.frameCount = len(self.thisFrame)

    #Lights move sequentially with decay
    class layerComet(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerComet, self).__init__(**kwargs)

            if self.effectCutoff <= 0: self.effectCutoff = .1

            decayTable = self._decayTable(factor=len(self.paletteColors)*self.effectCutoff,
                                          rate=1)

            gradient   = self.paletteColors[0] + self._gradient(self.paletteColors[1:],
                                                                len(decayTable)+1)

            decayTable = [c for b in zip(decayTable, decayTable, decayTable) for c in b]

            comet  = colorArray([a * b for a, b in zip(gradient,decayTable)])
  
            comet.padRight([0.0,0.0,0.0], self.ledCount)

            if self.direction: comet.reverse()

            for i in range(len(comet)):
                comet.shift(int(self.effectRate+(self.effectRate < 1)), self.direction)
                self.thisFrame.append(comet[0:self.ledCount])

                for x in range(int((1/self.effectRate)-(self.effectRate <= 1))):
                    self.thisFrame.append(comet[0:self.ledCount])
            
            self.frameCount = len(self.thisFrame)
     
    #Lights move sequentially with decay
    class layerChase(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerChase, self).__init__(**kwargs)

            decayTable = self._decayTable(factor=len(self.paletteColors)*self.effectCutoff,
                                          rate=1)

            gradient   = self.paletteColors[0] + self._gradient(self.paletteColors[1:],
                                                                len(decayTable)+1)

            decayTable = [c for b in zip(decayTable, decayTable, decayTable) for c in b]
            gradient  = colorArray([a * b 
                            for a, b in zip(gradient,decayTable)])

            chase = gradient

            for i in range(int(self.ledCount/len(gradient))):
                chase += gradient

            if self.direction: chase.reverse()

            for i in range(len(chase)):
                chase.shift(int(self.effectRate+(self.effectRate < 1)), self.direction)
                self.thisFrame.append(chase[0:self.ledCount])

                for x in range(int((1/self.effectRate)-(self.effectRate <= 1))):
                    self.thisFrame.append(chase[0:self.ledCount])

            self.frameCount = len(self.thisFrame)

    #Responds to heater temperature
    class layerGradient(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerGradient, self).__init__(**kwargs)

            self.paletteColors = colorArray(self.paletteColors + self.paletteColors[0])
            
            gradientLength = (3 - int(self.ledCount) % 3) + int(self.ledCount)
            gradient   = colorArray(self._gradient(self.paletteColors, gradientLength))
      
            for i in range(len(gradient)):
                gradient.shift(int(self.effectRate+(self.effectRate < 1)), self.direction)
                self.thisFrame.append(gradient[0:self.ledCount])

                for x in range(int((1/self.effectRate)-(self.effectRate <= 1))):
                    self.thisFrame.append(gradient[0:self.ledCount])            

            self.frameCount = len(self.thisFrame)
 
    #Responds to heater temperature
    class layerHeater(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerHeater, self).__init__(**kwargs)

            if len(self.paletteColors) == 1: 
                self.paletteColors += self.paletteColors

            gradient   = colorArray(self._gradient(self.paletteColors[:-1], 200) +
                                    self.paletteColors[-1:])
            
            for i in range(len(gradient)):
                self.thisFrame.append(gradient[i] * self.ledCount)
 
            self.frameCount = len(self.thisFrame)     
            
        def nextFrame(self, eventtime):
            if self.heaterTarget > 0.0:
                if self.heaterCurrent <= self.heaterTarget-5:
                    s = int((self.heaterCurrent / self.heaterTarget) * 200)
                    return self.thisFrame[s]
                else:
                    logging.info(self.thisFrame[-1])
                    return self.thisFrame[-1]
            elif self.effectRate > 0:
                if self.heaterCurrent >= self.effectRate:
                    s = int(((self.heaterCurrent - self.effectRate) 
                            / self.heaterLast) * 200)
                    return self.thisFrame[s]

            return None

    #Responds to analog pin voltage
    class layerAnalogPin(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerAnalogPin, self).__init__(**kwargs)

            if len(self.paletteColors) == 1: 
                self.paletteColors = [0.0,0.0,0.0] + self.paletteColors

            self.decayTable = self._decayTable(factor=8)
            self.gradient   = colorArray(self._gradient(self.paletteColors, 100))
            self.decayLen   = len(self.decayTable)
            self.decay      = 0

        def nextFrame(self, eventtime):    
            self.frameNumber = int(self.analogValue * self.effectRate) 

            if self.frameNumber > self.effectCutoff:
                self.decay = self.decayLen
                self.lastAnalog = self.frameNumber
                return self.gradient[self.frameNumber] * self.ledCount
            elif self.decay > 0:
                self.decay -= 1
                return self.gradient[int(self.decayTable[self.decay] * self.lastAnalog)] * self.ledCount

    #Lights illuminate relative to stepper position
    class layerStepper(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerStepper, self).__init__(**kwargs)

            if self.effectCutoff == 0:
                self.effectCutoff = self.ledCount

            if self.effectRate == 0:
                self.effectRate = self.ledCount

            if len(self.paletteColors) == 1: 
                self.paletteColors += self.paletteColors

            trailing    = colorArray(self._gradient(self.paletteColors, 
                                                   self.effectRate, True))

            leading     = colorArray(self._gradient(self.paletteColors, 
                                                    self.effectRate, False))

            trailing.padLeft([0.0,0.0,0.0], self.ledCount)
            leading.padRight([0.0,0.0,0.0], self.ledCount)
            gradient = colorArray(trailing + leading[1:])
            gradient.shift(len(trailing)-1, 0)
            frames = [gradient[:self.ledCount]]

            for i in range(0, self.ledCount):
                gradient.shift(1,1)
                frames.append(gradient[:self.ledCount])

            for i in range(100):
                x = int((i / 100.0) * self.ledCount)
                self.thisFrame.append(frames[x])

            self.frameCount = len(self.thisFrame) 

        def nextFrame(self, eventtime):
            return self.thisFrame[self.stepperPosition]

    #Shameless port of Fire2012 by Mark Kriegsman 
    # from the Arduino FastLED example files
    class layerFire(_layerBase):
        def __init__(self,  **kwargs):
            super(ledEffect.layerFire, self).__init__(**kwargs)

            self.heatMap    = [0.0] * self.ledCount
            self.gradient   = colorArray(self._gradient(self.paletteColors, 102))
            self.frameLen   = len(self.gradient)
            self.heatLen    = len(self.heatMap)
            self.heatSource = int(self.ledCount / 10.0)
            self.effectRate = int(100 - self.effectRate)

            if self.heatSource < 1:
                self.heatSource = 1

        def nextFrame(self, eventtime):
            frame = []

            for h in range(self.heatLen):
                c = randint(0,self.effectCutoff)
                self.heatMap[h] -= (self.heatMap[h] - c >= 0 ) * c

            for i in range(self.ledCount - 1, 2, -1):
                d = (self.heatMap[i - 1] + 
                     self.heatMap[i - 2] + 
                     self.heatMap[i - 3] ) / 3

                self.heatMap[i] = d * (d >= 0)

            if randint(0, 100) > self.effectRate:
                h = randint(0, self.heatSource)
                self.heatMap[h] += randint(90,100)
                if self.heatMap[h] > 100: 
                    self.heatMap[h] = 100

            for h in self.heatMap:
                frame += self.gradient[int(h)] 

            return frame

def load_config_prefix(config):
    return ledEffect(config)