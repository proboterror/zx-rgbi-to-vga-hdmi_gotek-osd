# -*- coding: utf-8 -*-

import sys
import time
import re
import glob
import serial
from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, 
                            QLabel, QComboBox, QPushButton, QSlider, QRadioButton, 
                            QCheckBox, QGroupBox, QFrame, QMessageBox, QGridLayout, QLineEdit)
from PyQt5.QtCore import Qt, QTimer, QRegExp
from PyQt5.QtGui import QRegExpValidator

class SliderWithInput(QWidget):
    """Виджет слайдера с полем ввода значения"""
    def __init__(self, orientation=Qt.Horizontal, min_val=0, max_val=100, parent=None):
        super().__init__(parent)
        self.min_val = min_val
        self.max_val = max_val
        
        self.layout = QHBoxLayout(self)
        self.slider = QSlider(orientation)
        self.slider.setRange(min_val, max_val)
        
        # Поле ввода с валидацией чисел
        self.value_edit = QLineEdit()
        self.value_edit.setFixedWidth(50)
        self.value_edit.setValidator(QRegExpValidator(QRegExp(r'^\d+$')))
        
        self.layout.addWidget(self.slider)
        self.layout.addWidget(self.value_edit)
        
        # Связываем сигналы
        self.slider.valueChanged.connect(self.update_edit_from_slider)
        self.value_edit.editingFinished.connect(self.update_slider_from_edit)
    
    def update_edit_from_slider(self, value):
        """Обновляем поле ввода при изменении слайдера"""
        self.value_edit.setText(str(value))
    
    def update_slider_from_edit(self):
        """Обновляем слайдер при изменении поля ввода"""
        text = self.value_edit.text()
        if text:
            try:
                value = int(text)
                if value < self.min_val:
                    value = self.min_val
                    self.value_edit.setText(str(value))
                elif value > self.max_val:
                    value = self.max_val
                    self.value_edit.setText(str(value))
                self.slider.setValue(value)
            except ValueError:
                pass
    
    def value(self):
        return self.slider.value()
    
    def setValue(self, value):
        self.slider.setValue(value)
        self.value_edit.setText(str(value))

class RGBtoVGAGUI(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('Настройки RGB to VGA & HDMI')
        self.setGeometry(100, 100, 700, 750)
        
        # Инициализация переменных
        self.ser = None
        self.oldSerName = None
        self.l_mode = 'read'
        self.x_sh = '0'
        self.y_sh = '0'
        self.delay_sh = '0'
        self.namesMode = ('самосинхронизация', 'внешняя частота')
        
        # Получение списка портов
        self.dev_TTY = self.serial_ports()
        if not self.dev_TTY:
            QMessageBox.critical(self, "Ошибка", "Не найдены последовательные порты")
            sys.exit()
            
        # Создание основного виджета и layout
        self.central_widget = QWidget()
        self.setCentralWidget(self.central_widget)
        self.main_layout = QVBoxLayout(self.central_widget)
        
        # Создание UI элементов
        self.create_video_output_block()
        self.create_port_selection_block()
        self.create_sync_block()
        self.create_pixel_clock_block()
        self.create_frequency_block()
        self.create_offset_blocks()
        self.create_delay_blocks()
        self.create_inversion_block()
        self.create_apply_button()
        
        # Таймер для проверки порта
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.check_serial_port)
        self.timer.start(100)
        
        # Инициализация значений
        self.init_values()
        
    def show_auto_close_message(self, title, message, timeout=2000):
        """Показать информационное сообщение, которое автозакрывается через timeout миллисекунд"""
        msg_box = QMessageBox(self)
        msg_box.setWindowTitle(title)
        msg_box.setText(message)
        msg_box.setStandardButtons(QMessageBox.NoButton)
        msg_box.show()
        QTimer.singleShot(timeout, msg_box.accept)       
     
    def serial_ports(self):
        """Список доступных последовательных портов"""
        if sys.platform.startswith('win'):
            ports = ['COM%s' % (i + 1) for i in range(256)]
        elif sys.platform.startswith('linux') or sys.platform.startswith('cygwin'):
            ports = glob.glob('/dev/tty[A-Za-z]*')
        elif sys.platform.startswith('darwin'):
            ports = glob.glob('/dev/tty.*')
        else:
            raise EnvironmentError('Unsupported platform')

        result = []
        for port in ports:
            try:
                s = serial.Serial(port)
                s.close()
                result.append(port)
            except (OSError, serial.SerialException):
                pass
        return result
    
    def create_video_output_block(self):
        """Блок настроек видеовыхода"""
        group = QGroupBox("Видеовыход")
        layout = QVBoxLayout()
        
        # Режимы вывода
        self.video_mode_combo = QComboBox()
        self.video_mode_combo.addItems([
            "DVI",
            "VGA 640x480",
            "VGA 800x600",
            "VGA 1024x768",
            "VGA 1280x1024 (div3)",
            "VGA 1280x1024 (div4)"
        ])
        
        # Scanlines режим
        self.is_scanlines = QCheckBox('Режим scanlines (эмуляция ЭЛТ)')
        
        # Буферизация
        self.is1X_BUFMODE = QRadioButton('1X буферизация')
        self.is3X_BUFMODE = QRadioButton('3X буферизация')
        self.is1X_BUFMODE.setChecked(True)
        
        buf_group = QGroupBox("Буферизация")
        buf_layout = QHBoxLayout()
        buf_layout.addWidget(self.is1X_BUFMODE)
        buf_layout.addWidget(self.is3X_BUFMODE)
        buf_group.setLayout(buf_layout)
        
        # Добавление элементов
        layout.addWidget(self.video_mode_combo)
        layout.addWidget(self.is_scanlines)
        
        # Добавление вложенных групп
        sub_layout = QHBoxLayout()
        sub_layout.addWidget(group)
        sub_layout.addWidget(buf_group)
        
        group.setLayout(layout)
        self.main_layout.addLayout(sub_layout)
        
        # Сигналы
        self.video_mode_combo.currentIndexChanged.connect(self.update_ui)
    
    def create_port_selection_block(self):
        """Блок выбора последовательного порта"""
        group = QGroupBox("Выбор порта")
        layout = QVBoxLayout()
        
        self.port_combo = QComboBox()
        self.port_combo.addItems(self.dev_TTY)
        
        layout.addWidget(self.port_combo)
        group.setLayout(layout)
        
        self.main_layout.addWidget(group)
    
    def create_sync_block(self):
        """Блок настроек синхронизации"""
        group = QGroupBox("Синхронизация")
        layout = QVBoxLayout()
        
        self.isEXT_sync = QRadioButton('Раздельная')
        self.isINT_sync = QRadioButton('Синхросмесь')
        self.isEXT_sync.setChecked(True)
        
        radio_layout = QHBoxLayout()
        radio_layout.addWidget(self.isEXT_sync)
        radio_layout.addWidget(self.isINT_sync)
        layout.addLayout(radio_layout)
        
        group.setLayout(layout)
        self.main_layout.addWidget(group)
        
        # Сигналы
        self.isEXT_sync.toggled.connect(self.update_ui)
        self.isINT_sync.toggled.connect(self.update_ui)
    
    def create_pixel_clock_block(self):
        """Блок источника пиксельклока"""
        group = QGroupBox("Источник пиксельклока")
        layout = QVBoxLayout()
        
        self.mode_combo = QComboBox()
        self.mode_combo.addItems(self.namesMode)
        
        # Делитель
        self.f_div_slider = SliderWithInput(Qt.Horizontal, 1, 5)
        div_group = QGroupBox("Делитель частоты")
        div_layout = QVBoxLayout()
        div_layout.addWidget(self.f_div_slider)
        div_group.setLayout(div_layout)
        
        layout.addWidget(self.mode_combo)
        layout.addWidget(div_group)
        group.setLayout(layout)
        
        self.main_layout.addWidget(group)
        
        # Сигналы
        self.mode_combo.currentIndexChanged.connect(self.update_ui)
    
    def create_frequency_block(self):
        """Блок внутренней частоты"""
        group = QGroupBox("Частота захвата (кГц)")
        layout = QVBoxLayout()
        
        self.f_slider = SliderWithInput(Qt.Horizontal, 6000, 8000)
        
        layout.addWidget(self.f_slider)
        group.setLayout(layout)
        
        self.main_layout.addWidget(group)
    
    def create_offset_blocks(self):
        """Блоки смещения X и Y"""
        frame = QFrame()
        layout = QHBoxLayout()
        
        # Смещение X
        self.x_slider = SliderWithInput(Qt.Horizontal, 0, 200)
        x_group = QGroupBox("Смещение X")
        x_layout = QVBoxLayout()
        x_layout.addWidget(self.x_slider)
        x_group.setLayout(x_layout)
        
        # Смещение Y
        self.y_slider = SliderWithInput(Qt.Horizontal, 0, 200)
        y_group = QGroupBox("Смещение Y")
        y_layout = QVBoxLayout()
        y_layout.addWidget(self.y_slider)
        y_group.setLayout(y_layout)
        
        layout.addWidget(x_group)
        layout.addWidget(y_group)
        frame.setLayout(layout)
        
        self.main_layout.addWidget(frame)
    
    def create_delay_blocks(self):
        """Блок задержки сигнала"""
        group = QGroupBox("Задержка сигнала")
        layout = QVBoxLayout()
        
        self.delay_slider = SliderWithInput(Qt.Horizontal, 0, 31)
        
        layout.addWidget(self.delay_slider)
        group.setLayout(layout)
        
        self.main_layout.addWidget(group)
    
    def create_inversion_block(self):
        """Блок инвертирования входных сигналов"""
        group = QGroupBox("Инвертирование входных сигналов (биты маски 0b76543210)")
        layout = QGridLayout()
        
        # Бит 7 (0b10000000) - OSD
        self.is_inv_OSD = QCheckBox('7: OSD')
        layout.addWidget(self.is_inv_OSD, 0, 0)
        
        # Бит 6 (0b01000000) - CLK (частота)
        self.is_inv_CLK = QCheckBox('6: CLK (частота)')
        layout.addWidget(self.is_inv_CLK, 0, 1)
        
        # Бит 5 (0b00100000) - VS (вертикальная синхронизация)
        self.is_inv_VS = QCheckBox('5: VS (КСИ)')
        layout.addWidget(self.is_inv_VS, 1, 0)
        
        # Бит 4 (0b00010000) - HS (горизонтальная синхронизация)
        self.is_inv_HS = QCheckBox('4: HS (ССИ)')
        layout.addWidget(self.is_inv_HS, 1, 1)
        
        # Бит 3 (0b00001000) - I (сигнал яркости)
        self.is_inv_I = QCheckBox('3: I (яркость)')
        layout.addWidget(self.is_inv_I, 2, 0)
        
        # Бит 2 (0b00000100) - R (красный)
        self.is_inv_R = QCheckBox('2: R (красный)')
        layout.addWidget(self.is_inv_R, 2, 1)
        
        # Бит 1 (0b00000010) - G (зеленый)
        self.is_inv_G = QCheckBox('1: G (зеленый)')
        layout.addWidget(self.is_inv_G, 3, 0)
        
        # Бит 0 (0b00000001) - B (синий)
        self.is_inv_B = QCheckBox('0: B (синий)')
        layout.addWidget(self.is_inv_B, 3, 1)
        
        group.setLayout(layout)
        self.main_layout.addWidget(group)
    
    def create_apply_button(self):
        """Кнопка применения настроек"""
        self.apply_btn = QPushButton('Применить')
        self.apply_btn.clicked.connect(self.apply_settings)
        self.main_layout.addWidget(self.apply_btn)
    
    def init_values(self):
        """Инициализация начальных значений"""
        self.update_ui()
    
    def update_ui(self):
        """Обновление видимости элементов интерфейса"""
        mode_index = self.mode_combo.currentIndex()
        
        # Обновление видимости блоков в зависимости от режима
        self.f_div_slider.parent().setVisible(mode_index == 1)  # внешняя частота
        self.f_slider.parent().setVisible(mode_index == 0)     # самосинхронизация
        
        # Для режима DVI отключаем scanlines
        video_mode = self.video_mode_combo.currentIndex()
        self.is_scanlines.setEnabled(video_mode != 0)
    
    def check_serial_port(self):
        """Проверка и обновление последовательного порта"""
        current_port = self.port_combo.currentText()
        
        if current_port != self.oldSerName:
            if self.l_mode != 'write':
                self.l_mode = 'read'
            
            if self.ser is None:
                self.connect_to_serial(current_port)
            else:
                self.ser.close()
                self.connect_to_serial(current_port)
        
        # Чтение/запись данных
        if self.ser is not None:
            try:
                self.ser.write("mode\n".encode())
                time.sleep(0.1)
                hw_mode = str(self.ser.readall())
                
                if "mode 0" in hw_mode:
                    self.handle_mode0()
                elif "mode 1" in hw_mode:
                    self.handle_mode1()
                
            except Exception as e:
                print(f"Serial error: {e}")
                if self.ser:
                    self.ser.close()
                self.ser = None
                self.oldSerName = None
    
    def connect_to_serial(self, port):
        """Подключение к последовательному порту"""
        while self.ser is None:
            try:
                self.ser = serial.Serial(port, 115200, timeout=0.001)
                self.oldSerName = port
            except Exception as e:
                print(f"Failed to connect to {port}: {e}")
                time.sleep(1)
    
    def handle_mode0(self):
        """Обработка режима 0 (чтение/запись)"""
        if self.l_mode == 'write':
            self.write_settings()
        elif self.l_mode == 'read':
            self.read_settings()
    
    def handle_mode1(self):
        """Обработка режима 1 (только смещение)"""
        if self.l_mode != 'user_mode':
            self.ser.write("reset\n".encode())
            time.sleep(0.1)
            return
        
        xnew = self.x_slider.value()
        ynew = self.y_slider.value()
        delaynew = self.delay_slider.value()
        
        if self.x_sh != xnew:
            cmd = f"wcap_sh_x {xnew}\n"
            self.ser.write(cmd.encode())
            self.x_sh = xnew
            time.sleep(0.1)
            print(str(self.ser.readall()))
        
        if self.y_sh != ynew:
            cmd = f"wcap_sh_y {ynew}\n"
            self.ser.write(cmd.encode())
            self.y_sh = ynew
            time.sleep(0.1)
            print(str(self.ser.readall()))
            
        if self.delay_sh != delaynew:
            cmd = f"wdelay {delaynew}\n"
            self.ser.write(cmd.encode())
            self.delay_sh = delaynew
            time.sleep(0.1)
            print(str(self.ser.readall()))
 
 
    def write_settings(self):
        """Запись настроек в устройство"""
        self.show_auto_close_message("Информация", "Сохранение параметров", 2000)
        
        commands = [
            f"wcap_sh_x {self.x_slider.value()}\n",
            f"wcap_sh_y {self.y_slider.value()}\n",
            f"wext_clk_divider {self.f_div_slider.value()}\n",
            f"wfrequency {self.f_slider.value()}000\n",
            f"wdelay {self.delay_slider.value()}\n",
            f"wvideo_out_mode {self.video_mode_combo.currentIndex()}\n",
            f"wscanlines_mode {1 if self.is_scanlines.isChecked() else 0}\n",
            f"wx3_buffering_mode {1 if self.is3X_BUFMODE.isChecked() else 0}\n",
            f"wcap_sync_mode {1 if self.isEXT_sync.isChecked() else 0}\n"
        ]
        
        # Режим пиксельклока
        mode_index = self.mode_combo.currentIndex()
        if mode_index == 1: 
            commands.append("wcap_sync_mode 1\n")  # EXT
        else: 
            commands.append("wcap_sync_mode 0\n")  # INT
        
        # Формирование маски инверсии:
        inv_mask = 0
        if self.is_inv_OSD.isChecked(): inv_mask |= 0b10000000  # Бит 7
        if self.is_inv_CLK.isChecked(): inv_mask |= 0b01000000  # Бит 6
        if self.is_inv_VS.isChecked(): inv_mask |= 0b00100000   # Бит 5
        if self.is_inv_HS.isChecked(): inv_mask |= 0b00010000   # Бит 4
        if self.is_inv_I.isChecked(): inv_mask |= 0b00001000    # Бит 3 
        if self.is_inv_R.isChecked(): inv_mask |= 0b00000100    # Бит 2
        if self.is_inv_G.isChecked(): inv_mask |= 0b00000010    # Бит 1
        if self.is_inv_B.isChecked(): inv_mask |= 0b00000001    # Бит 0
    
        commands.append(f"wpin_inversion_mask {inv_mask}\n")
        
        # Отправка команд
        for cmd in commands:
            self.ser.write(cmd.encode())
            time.sleep(0.01)
            self.ser.readall()
        
        # Сохранение и выход
        self.l_mode = 'user_mode'
        self.ser.write("save\n".encode())
        
    def read_settings(self):
        """Чтение настроек из устройства"""
        self.show_auto_close_message("Информация", "Загрузка параметров", 2000)
        
        # Чтение значений
        settings = {
            'x_offset': self.read_serial_value("rcap_sh_x\n"),
            'y_offset': self.read_serial_value("rcap_sh_y\n"),
            'f_div': self.read_serial_value("rext_clk_divider\n"),
            'freq': self.read_serial_value("rfrequency\n"),
            'delay': self.read_serial_value("rdelay\n"),
            'video_out': self.read_serial_value("rvideo_out_mode\n"),
            'scanlines': self.read_serial_value("rscanlines_mode\n"),
            'buf_mode': self.read_serial_value("rx3_buffering_mode\n"),
            'sync_mode': self.read_serial_value("rcap_sync_mode\n"),
            'inv_mask': self.read_serial_value("rpin_inversion_mask\n")
        }
        
        # Установка значений в UI
        self.x_slider.setValue(settings['x_offset'])
        self.y_slider.setValue(settings['y_offset'])
        self.f_div_slider.setValue(settings['f_div'])
        self.f_slider.setValue(settings['freq'] // 1000)
        self.delay_slider.setValue(settings['delay'])
        
        # Видеовыход
        self.video_mode_combo.setCurrentIndex(settings['video_out'])
        
        # Scanlines режим
        self.is_scanlines.setChecked(settings['scanlines'] == 1)
        
        # Буферизация
        self.is1X_BUFMODE.setChecked(settings['buf_mode'] == 0)
        self.is3X_BUFMODE.setChecked(settings['buf_mode'] == 1)
        
        # Режим синхронизации
        self.isEXT_sync.setChecked(settings['sync_mode'] == 1)
        self.isINT_sync.setChecked(settings['sync_mode'] == 0)
        
        # Режим пиксельклока
        mode_index = 0
        if settings['sync_mode'] == 1: mode_index = 1
        self.mode_combo.setCurrentIndex(mode_index)
        
        # Разбор маски инверсии:
        inv_mask = settings['inv_mask']
        self.is_inv_OSD.setChecked(bool(inv_mask & 0b10000000))  # Бит 7
        self.is_inv_CLK.setChecked(bool(inv_mask & 0b01000000))  # Бит 6
        self.is_inv_VS.setChecked(bool(inv_mask & 0b00100000))   # Бит 5
        self.is_inv_HS.setChecked(bool(inv_mask & 0b00010000))   # Бит 4
        self.is_inv_I.setChecked(bool(inv_mask & 0b00001000))    # Бит 3
        self.is_inv_R.setChecked(bool(inv_mask & 0b00000100))    # Бит 2
        self.is_inv_G.setChecked(bool(inv_mask & 0b00000010))    # Бит 1
        self.is_inv_B.setChecked(bool(inv_mask & 0b00000001))    # Бит 0
        
        self.l_mode = 'user_mode'
        self.ser.write("exit\n".encode())
    
    def read_serial_value(self, command):
        """Чтение значения из последовательного порта"""
        self.ser.write(command.encode())
        time.sleep(0.01)
        response = str(self.ser.readall())
        return int(''.join(x for x in response if x.isdigit()))
    
    def apply_settings(self):
        """Обработка нажатия кнопки Применить"""
        self.l_mode = 'write'
        self.show_auto_close_message("Информация", "Перезагрузка платы, ждите...", 2000)

if __name__ == '__main__':
    app = QApplication(sys.argv)
    window = RGBtoVGAGUI()
    window.show()
    sys.exit(app.exec_())