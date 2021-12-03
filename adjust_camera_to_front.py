from servoserial import ServoSerial

servo_device = ServoSerial() 

# adjust the camera to front position
servo_device.Servo_serial_double_control(1, 2100, 2, 1500)
