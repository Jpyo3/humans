#!/usr/bin/python
#
# Copyright (c) 2009, Georgia Tech Research Corporation
# All rights reserved.
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of the Georgia Tech Research Corporation nor the
#       names of its contributors may be used to endorse or promote products
#       derived from this software without specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY GEORGIA TECH RESEARCH CORPORATION ''AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL GEORGIA TECH BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
# OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
# OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

## Controlling Robotis Dynamixel RX-28 & RX-64 servos from python
## using the USB2Dynamixel adaptor.

## Authors: Travis Deyle & Advait Jain (Healthcare Robotics Lab, Georgia Tech.)

import serial
import time
import thread
import sys, optparse
import servo_config as sc
import math

class USB2Dynamixel_Device():
    ''' Class that manages serial port contention between servos on same bus
    '''
    def __init__( self, dev_name = '/dev/ttyUSB0', baudrate = 57600 ):
        try:
            self.dev_name = string.atoi( dev_name ) # stores the serial port as 0-based integer for Windows
        except:
            self.dev_name = dev_name # stores it as a /dev-mapped string for Linux / Mac

        self.mutex = thread.allocate_lock()
        self.servo_dev = None

        self.acq_mutex()
        self._open_serial( baudrate )
        self.rel_mutex()

    def acq_mutex(self):
        self.mutex.acquire()

    def rel_mutex(self):
        self.mutex.release()

    def send_serial(self, msg):
        # It is up to the caller to acquire / release mutex
        self.servo_dev.write( msg )

    def read_serial(self, nBytes=1):
        # It is up to the caller to acquire / release mutex
        rep = self.servo_dev.read( nBytes )
        return rep

    def _open_serial(self, baudrate):
        try:
            self.servo_dev = serial.Serial(self.dev_name, timeout=1.)
            self.servo_dev.setBaudrate(baudrate)
            self.servo_dev.setParity('N')
            self.servo_dev.setStopbits(1)
            self.servo_dev.open()

            self.servo_dev.flushOutput()
            self.servo_dev.flushInput()

        except (serial.serialutil.SerialException), e:
            raise RuntimeError('lib_robotis_rx: Serial port not found!\n')
        if(self.servo_dev == None):
            raise RuntimeError('lib_robotis_rx: Serial port not found!\n')





class Robotis_Servo():
    ''' Class to use a robotis RX-28 or RX-64 servo.
    '''
    def __init__(self, USB2Dynamixel, servo_id ):
        ''' USB2Dynamixel - USB2Dynamixel_Device object to handle serial port.
                            Handles threadsafe operation for multiple servos
            servo_id - servo ids connected to USB2Dynamixel 1,2,3,4 ... (1 to 253)
                       [0 is broadcast if memory serves]
        '''

        # To change the defaults, load some or all changes into servo_config.py
        defaults = {
            'home_encoder': 0x200,
            'rad_per_enc': math.radians(300.0) / 1024.0, 
            'max_ang': math.radians(148),
            'min_ang': math.radians(-148),
            'flipped': False,
            'max_speed': math.radians(50)
            }

        # Error Checking
        if USB2Dynamixel == None:
            raise RuntimeError('lib_robotis_rx: Robotis Servo requires USB2Dynamixel!\n')
        else:
            self.dyn = USB2Dynamixel

        # ID exists on bus?
        self.servo_id = servo_id
        if self.read_address(3) == []:
            raise RuntimeError('lib_robotis_rx: Could not find ID (%d) on bus (%s)\n' %
                               ( servo_id, self.dyn.dev_name ))

        # Set Return Delay time - Used to determine when next status can be requested
        data = self.read_address( 0x05, 1)
        self.return_delay = data[0] * 2e-6

        # Set various parameters.  Load from servo_config.
        self.settings = {}
        try:
            import servo_config as sc
            if sc.servo_param.has_key( self.servo_id ):
                self.settings = sc.servo_param[ self.servo_id ]
            else:
                raise RuntimeError('')
        except:
            print 'servo_config.py not found or servo_id not present.  Applying defaults.'

        # Set to default any parameter not specified in servo_config
        for key in defaults.keys():
            if self.settings.has_key( key ):
                pass
            else:
                self.settings[ key ] = defaults[ key ]


    def is_moving(self):
        ''' returns True if servo is moving.
        '''
        data = self.read_address( 0x2e, 1 )
        return data[0] != 0

    def read_voltage(self):
        ''' returns voltage (Volts)
        '''
        data = self.read_address( 0x2a, 1 )
        return data[0] / 10.

    def read_temperature(self):
        ''' returns the temperature (Celcius)
        '''
        data = self.read_address( 0x2b, 1 )
        return data[0]

    def read_load(self):
        ''' number proportional to the torque applied by the servo.
            sign etc. might vary with how the servo is mounted.
        '''
        data = self.read_address( 0x28, 2 )
        load = data[0] + (data[1] >> 6) * 256
        if data[1] >> 2 & 1 == 0:
            return -1.0 * load
        else:
            return 1.0 * load

    def read_encoder(self):
        ''' returns position in encoder ticks
        '''
        data = self.read_address( 0x24, 2 )
        enc_val = data[0] + data[1] * 256
        return enc_val

    def read_angle(self):
        ''' returns the current servo angle (radians)
        '''
        ang = (self.read_encoder() - self.settings['home_encoder']) * self.settings['rad_per_enc']
        if self.settings['flipped']:
            ang = ang * -1.0
        return ang

    def move_angle(self, ang, angvel=None, blocking=True):
        ''' move to angle (radians)
        '''
        if angvel == None:
            angvel = self.settings['max_speed']

        if angvel > self.settings['max_speed']:
            print 'lib_robotis_rx.move_angle: angvel too high - %.2f deg/s' % (math.degrees(angvel))
            print 'lib_robotis_rx.ignoring move command.'
            return

        if ang > self.settings['max_ang'] or ang < self.settings['min_ang']:
            print 'lib_robotis_rx.move_angle: angle out of range- ', math.degrees(ang)
            print 'lib_robotis_rx.ignoring move command.'
            return
        
        self.set_angvel(angvel)

        if self.settings['flipped']:
            ang = ang * -1.0
        enc_tics = int(round( ang / self.settings['rad_per_enc'] ))
        enc_tics += self.settings['home_encoder']
        self.move_to_encoder( enc_tics )

        if blocking == True:
            while(self.is_moving()):
                continue

    def move_to_encoder(self, n):
        ''' move to encoder position n
        '''
        hi,lo = n / 256, n % 256
        return self.write_address( 0x1e, [lo,hi] )

    def enable_torque(self):
        return self.write_address(0x18, [1])

    def disable_torque(self):
        return self.write_address(0x18, [0])

    def set_angvel(self, angvel):
        ''' angvel - in rad/sec
        '''
        rpm = angvel / (2 * math.pi) * 60.0
        angvel_enc = int(round( rpm / 0.111 ))
        hi,lo = angvel_enc / 256, angvel_enc % 256
        return self.write_address( 0x20, [lo,hi] )

    def write_id(self, id):
        ''' changes the servo id
        '''
        return self.write_address( 0x03, [id] )

    def __calc_checksum(self, msg):
        chksum = 0
        for m in msg:
            chksum += m
        chksum = ( ~chksum ) % 256
        return chksum

    def read_address(self, address, nBytes=1):
        ''' reads nBytes from address on the servo.
            returns [n1,n2 ...] (list of parameters)
        '''
        msg = [ 0x02, address, nBytes ]
        return self.send_instruction( msg, self.servo_id )

    def write_address(self, address, data):
        ''' writes data at the address.
            data = [n1,n2 ...] list of numbers.
            return [n1,n2 ...] (list of return parameters)
        '''
        msg = [ 0x03, address ] + data
        return self.send_instruction( msg, self.servo_id )

    def send_instruction(self, instruction, id):
        msg = [ id, len(instruction) + 1 ] + instruction # instruction includes the command (1 byte + parameters. length = parameters+2)
        chksum = self.__calc_checksum( msg )
        msg = [ 0xff, 0xff ] + msg + [chksum]
        
        self.dyn.acq_mutex()
        self.send_serial( msg )
        data, err = self.receive_reply()
        self.dyn.rel_mutex()
        
        if err != 0:
            self.process_err( err )

        return data

    def process_err( self, err ):
        raise RuntimeError('lib_robotis_rx: An error occurred: %d\n' % err)

    def receive_reply(self):
        start = self.dyn.read_serial( 2 )
        if start != '\xff\xff':
            raise RuntimeError('lib_robotis_rx: Failed to receive start bytes\n')
        servo_id = self.dyn.read_serial( 1 )
        if ord(servo_id) != self.servo_id:
            raise RuntimeError('lib_robotis_rx: Incorrect servo ID received: %d\n' % ord(servo_id))
        data_len = self.dyn.read_serial( 1 )
        err = self.dyn.read_serial( 1 )
        data = self.dyn.read_serial( ord(data_len) - 2 )
        checksum = self.dyn.read_serial( 1 ) # I'm not going to check...
        return [ord(v) for v in data], ord(err)
        

    def send_serial(self, msg):
        """ sends the command to the servo
        """
        out = ''
        for m in msg:
            out += chr(m)
        self.dyn.send_serial( out )


if __name__ == '__main__':

    dyn = USB2Dynamixel_Device('/dev/robot/servo_left')
    pan = Robotis_Servo( dyn, 11 )
    tilt = Robotis_Servo( dyn, 12 )

    t0 = time.time()
    for i in xrange(200):
        ang = pan.read_angle()
    tt = time.time() - t0
    print 'Avg time per read: ', tt / 200.

#     p = optparse.OptionParser()
#     p.add_option('-d', action='store', type='string', dest='servo_dev_name',
#                  default='/dev/robot/servo0', help='servo device string. [default= /dev/robot/servo0]')
#     p.add_option('--ang', action='store', type='float', dest='ang',
#                  help='angle to move the servo to (degrees).')
#     p.add_option('--ang_vel', action='store', type='float', dest='ang_vel',
#                  help='angular velocity. (degrees/sec) [default = 50]', default=50)
#     p.add_option('--id', action='store', type='int', dest='id',
#                  help='id of servo to connect to, [default = 2]', default=2)

#     opt, args = p.parse_args()
#     servo_dev_name = opt.servo_dev_name
#     ang = opt.ang
#     ang_vel = opt.ang_vel
#     id = opt.id

#     servo = robotis_servo(servo_dev_name,id)
#     servo.move_angle(math.radians(ang), math.radians(ang_vel))
#     time.sleep(0.5)
#     print 'Servo angle:', math.degrees(servo.read_angle())


