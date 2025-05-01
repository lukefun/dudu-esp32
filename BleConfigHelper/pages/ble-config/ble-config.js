// pages/ble-config/ble-config.js
const app = getApp()

// 定义蓝牙服务和特征值 UUID
const WIFI_CONFIG_SERVICE_UUID = "CDB7950D-73F1-4D4D-8E47-C090502DBD63"
const SSID_CHAR_UUID = "CDB7950D-73F1-4D4D-8E47-C090502DBD64"
const PASSWORD_CHAR_UUID = "CDB7950D-73F1-4D4D-8E47-C090502DBD65"
const CONTROL_STATUS_CHAR_UUID = "CDB7950D-73F1-4D4D-8E47-C090502DBD66"

// WiFi 连接状态码
const WIFI_STATUS = {
  IDLE: 0x00,
  CONNECTING: 0x01,
  CONNECTED: 0x02,
  FAIL: 0x03,
  WEAK_SIGNAL: 0x04,
  FAIL_AUTH: 0x05,
  FAIL_AP_NOT_FOUND: 0x06,
  FAIL_CONN: 0x07,
  FAIL_SSID: 0x08,
  FAIL_OTHER: 0x09
}

// 控制命令
const WIFI_CONTROL_CMD = {
  CONNECT: 0xFF
}

Page({
  data: {
    scanning: false,
    devices: [],
    connected: false,
    deviceId: '',
    serviceId: '',
    ssid: '',
    password: '',
    showPassword: false,
    connecting: false,
    statusMessage: '',
    hasError: false
  },

  onLoad() {
    // 检查蓝牙适配器是否可用
    wx.openBluetoothAdapter({
      success: (res) => {
        console.log('蓝牙适配器已就绪')
        this.startBluetoothDevicesDiscovery()
      },
      fail: (res) => {
        wx.showModal({
          title: '提示',
          content: '请开启蓝牙功能',
          showCancel: false
        })
      }
    })
  },

  onUnload() {
    this.stopBluetoothDevicesDiscovery()
    if (this.data.connected) {
      this.disconnectBle()
    }
  },

  // 开始扫描蓝牙设备
  startBluetoothDevicesDiscovery() {
    if (this.data.scanning) return
    
    this.setData({ scanning: true, devices: [] })
    
    wx.startBluetoothDevicesDiscovery({
      allowDuplicatesKey: false,
      success: (res) => {
        console.log('开始扫描蓝牙设备')
        this.onBluetoothDeviceFound()
      },
      fail: (res) => {
        console.error('扫描蓝牙设备失败:', res)
        this.setData({ scanning: false })
      }
    })
  },

  // 停止扫描蓝牙设备
  stopBluetoothDevicesDiscovery() {
    wx.stopBluetoothDevicesDiscovery()
    this.setData({ scanning: false })
  },

  // 监听发现新设备事件
  onBluetoothDeviceFound() {
    wx.onBluetoothDeviceFound((res) => {
      res.devices.forEach(device => {
        console.log('发现设备:', device.name || device.localName || '未知设备')
        // 只显示目标设备
        if (device.name !== 'DuDu-BLE' && device.localName !== 'DuDu-BLE') {
          return
        }
        // 检查是否已存在该设备
        const idx = this.data.devices.findIndex(item => item.deviceId === device.deviceId)
        if (idx === -1) {
          this.data.devices.push(device)
          this.setData({ devices: this.data.devices })
        }
      })
    })
  },

  // 连接设备
  connectDevice(e) {
    const deviceId = e.currentTarget.dataset.deviceId
    this.setData({ deviceId })
    this.stopBluetoothDevicesDiscovery()

    wx.createBLEConnection({
      deviceId,
      success: (res) => {
        this.setData({ connected: true })
        this.getBleServices(deviceId)
      },
      fail: (res) => {
        console.error('连接设备失败:', res)
        wx.showToast({
          title: '连接失败',
          icon: 'none'
        })
      }
    })
  },

  // 获取设备服务
  getBleServices(deviceId) {
    wx.getBLEDeviceServices({
      deviceId,
      success: (res) => {
        for (let i = 0; i < res.services.length; i++) {
          if (res.services[i].uuid.toUpperCase() === WIFI_CONFIG_SERVICE_UUID) {
            this.setData({ serviceId: res.services[i].uuid })
            this.getBleCharacteristics()
            break
          }
        }
      },
      fail: (res) => {
        console.error('获取服务失败:', res)
      }
    })
  },

  // 获取特征值
  getBleCharacteristics() {
    wx.getBLEDeviceCharacteristics({
      deviceId: this.data.deviceId,
      serviceId: this.data.serviceId,
      success: (res) => {
        // 订阅状态通知
        this.notifyBleCharacteristicValueChange()
      },
      fail: (res) => {
        console.error('获取特征值失败:', res)
      }
    })
  },

  // 订阅状态通知
  notifyBleCharacteristicValueChange() {
    wx.notifyBLECharacteristicValueChange({
      deviceId: this.data.deviceId,
      serviceId: this.data.serviceId,
      characteristicId: CONTROL_STATUS_CHAR_UUID,
      state: true,
      success: (res) => {
        // 监听特征值变化
        this.onBleCharacteristicValueChange()
      },
      fail: (res) => {
        console.error('订阅状态通知失败:', res)
      }
    })
  },

  // 监听特征值变化
  onBleCharacteristicValueChange() {
    wx.onBLECharacteristicValueChange((res) => {
      if (res.characteristicId === CONTROL_STATUS_CHAR_UUID) {
        const value = new Uint8Array(res.value)
        this.handleStatusChange(value[0])
      }
    })
  },

  // 处理状态变化
  handleStatusChange(status) {
    let statusMessage = ''
    let hasError = false

    switch (status) {
      case WIFI_STATUS.IDLE:
        statusMessage = '等待配网...'
        break
      case WIFI_STATUS.CONNECTING:
        statusMessage = '正在连接WiFi...'
        break
      case WIFI_STATUS.CONNECTED:
        statusMessage = 'WiFi连接成功！'
        this.setData({ connecting: false })
        wx.showToast({
          title: 'WiFi连接成功',
          icon: 'success'
        })
        break
      case WIFI_STATUS.FAIL:
        statusMessage = 'WiFi连接失败'
        hasError = true
        break
      case WIFI_STATUS.WEAK_SIGNAL:
        statusMessage = 'WiFi信号太弱'
        hasError = true
        break
      case WIFI_STATUS.FAIL_AUTH:
        statusMessage = 'WiFi密码错误'
        hasError = true
        break
      case WIFI_STATUS.FAIL_AP_NOT_FOUND:
        statusMessage = '找不到该WiFi'
        hasError = true
        break
      case WIFI_STATUS.FAIL_CONN:
        statusMessage = 'WiFi连接失败'
        hasError = true
        break
      case WIFI_STATUS.FAIL_SSID:
        statusMessage = 'WiFi名称无效'
        hasError = true
        break
      case WIFI_STATUS.FAIL_OTHER:
        statusMessage = '未知错误'
        hasError = true
        break
    }

    this.setData({ 
      statusMessage,
      hasError,
      connecting: status === WIFI_STATUS.CONNECTING
    })

    if (hasError) {
      wx.showToast({
        title: statusMessage,
        icon: 'none'
      })
    }
  },

  // 写入 SSID
  writeSsid() {
    if (!this.data.ssid) {
      wx.showToast({
        title: '请输入WiFi名称',
        icon: 'none'
      })
      return
    }

    const buffer = new ArrayBuffer(this.data.ssid.length)
    const dataView = new DataView(buffer)
    for (let i = 0; i < this.data.ssid.length; i++) {
      dataView.setUint8(i, this.data.ssid.charCodeAt(i))
    }

    wx.writeBLECharacteristicValue({
      deviceId: this.data.deviceId,
      serviceId: this.data.serviceId,
      characteristicId: SSID_CHAR_UUID,
      value: buffer,
      success: (res) => {
        console.log('SSID写入成功')
        this.writePassword()
      },
      fail: (res) => {
        console.error('SSID写入失败:', res)
        wx.showToast({
          title: 'SSID写入失败',
          icon: 'none'
        })
      }
    })
  },

  // 写入密码
  writePassword() {
    if (!this.data.password) {
      wx.showToast({
        title: '请输入WiFi密码',
        icon: 'none'
      })
      return
    }

    const buffer = new ArrayBuffer(this.data.password.length)
    const dataView = new DataView(buffer)
    for (let i = 0; i < this.data.password.length; i++) {
      dataView.setUint8(i, this.data.password.charCodeAt(i))
    }

    wx.writeBLECharacteristicValue({
      deviceId: this.data.deviceId,
      serviceId: this.data.serviceId,
      characteristicId: PASSWORD_CHAR_UUID,
      value: buffer,
      success: (res) => {
        console.log('密码写入成功')
        this.sendConnectCommand()
      },
      fail: (res) => {
        console.error('密码写入失败:', res)
        wx.showToast({
          title: '密码写入失败',
          icon: 'none'
        })
      }
    })
  },

  // 发送连接命令
  sendConnectCommand() {
    const buffer = new ArrayBuffer(1)
    const dataView = new DataView(buffer)
    dataView.setUint8(0, WIFI_CONTROL_CMD.CONNECT)

    wx.writeBLECharacteristicValue({
      deviceId: this.data.deviceId,
      serviceId: this.data.serviceId,
      characteristicId: CONTROL_STATUS_CHAR_UUID,
      value: buffer,
      success: (res) => {
        console.log('连接命令发送成功')
        this.setData({ connecting: true })
      },
      fail: (res) => {
        console.error('连接命令发送失败:', res)
        wx.showToast({
          title: '连接命令发送失败',
          icon: 'none'
        })
      }
    })
  },

  // 断开连接
  disconnectBle() {
    wx.closeBLEConnection({
      deviceId: this.data.deviceId,
      success: (res) => {
        this.setData({
          connected: false,
          deviceId: '',
          serviceId: '',
          ssid: '',
          password: '',
          statusMessage: '',
          hasError: false,
          connecting: false
        })
      }
    })
  },

  // 刷新设备列表
  refreshDevices() {
    this.setData({ devices: [] })
    this.startBluetoothDevicesDiscovery()
  },

  // 切换密码显示状态
  togglePasswordVisibility() {
    this.setData({
      showPassword: !this.data.showPassword
    })
  },

  // 输入框事件处理
  onSsidInput(e) {
    this.setData({
      ssid: e.detail.value
    })
  },

  onPasswordInput(e) {
    this.setData({
      password: e.detail.value
    })
  },

  // 提交配网
  submitConfig() {
    if (!this.data.ssid || !this.data.password) {
      wx.showToast({
        title: '请输入WiFi名称和密码',
        icon: 'none'
      })
      return
    }
    this.writeSsid()
  }
})