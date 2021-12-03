'''
@Author: Malloy.Yuan
@Wechat: ymlin96546
@Date: 2019-07-18 21:37:30
@LastEditors: Malloy.Yuan
@LastEditTime: 2019-08-08 14:43:33
'''
#-*- coding:UTF-8 -*-
import RPi.GPIO as GPIO
import time
import string
import serial

#设置GPIO口为BCM编码方式
GPIO.setmode(GPIO.BCM)

class ServoSerial:
    def __init__(self):
        self.ser = serial.Serial("/dev/ttyTHS1", 115200, timeout = 0.001)
        print ("serial Open!")
    
    def __del__(self):
        self.ser.close()
        print ("serial Close!")

    def Servo_serial_control(self, index, angle):
        pack1 = 0xff
        pack2 = 0xff
        id = index
        len = 0x07
        cmd = 0x03
        addr = 0x2A
        #将舵机转动脉冲保持在安全范围内
        if index == 1:
            if angle<600:
                angle=600
            elif angle>3600:
                angle = 3600
        elif index == 2:
            if angle<1300:
                angle=1300
            elif angle>4095:
                angle=4095

        pos_H = (angle >> 8) & 0x00ff
        pos_L = angle & 0x00ff
        
        time_H = 0x00   # time = 500ms 分解成两个
        time_L = 0x0A
        checknum = (~(id + len + cmd + addr + pos_H + pos_L + time_H + time_L)) & 0xff
#         print(checknum)
        data = [pack1, pack2, id, len, cmd, addr, pos_H, pos_L, time_H, time_L, checknum]
#         print(bytes(data))
        self.ser.write(bytes(data))

    def Servo_serial_double_control(self, index_1, angle_1, index_2, angle_2):
        pack1 = 0xff
        pack2 = 0xff
        id = 0xFE
        id_1 = index_1
        id_2 = index_2
        len = 0x0E
        cmd = 0x83
        addr1 = 0x2A
        addr2 = 0x04
        #将舵机转动脉冲保持在安全范围内
        if angle_1<600:
            angle_1=600
        elif angle_1>3600:
            angle_1 = 3600

        if angle_2<1300:
            angle_2=1300
        elif angle_2>4095:
            angle_2=4095

        pos1_H = (angle_1 >> 8) & 0x00ff
        pos1_L = angle_1 & 0x00ff

        pos2_H = (angle_2 >> 8) & 0x00ff
        pos2_L = angle_2 & 0x00ff
        
        time1_H = 0x00   # time = 10ms 分解成两个
        time1_L = 0x0A

        time2_H = 0x00   # time = 10ms 分解成两个
        time2_L = 0x0A

        checknum = (~(id + len + cmd + addr1 + addr2 + id_1 + pos1_H + pos1_L + time1_H + time1_L + id_2 + pos2_H + pos2_L + time2_H + time2_L)) & 0xff
#         print(checknum)
        data = [pack1, pack2, id, len, cmd, addr1, addr2, id_1, pos1_H, pos1_L, time1_H, time1_L, id_2, pos2_H, pos2_L, time2_H, time2_L, checknum]
#         print(bytes(data))
        self.ser.write(bytes(data))

	
	
	
	
	
	
