#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/regmap.h>

#define DS90UB941_I2C_ADDR 0x0c
#define DS90UB948_I2C_ADDR 0x2c

#define RESET_MDELAY 50
#define I2C_RW_MDELAY 50

#define RETRY_TIL_ZERO_COUNT 10
#define REGMAP_RETRY_COUNT 10

#define RETRY_TIL_ZERO( exp )				\
({							\
	int _attemptc_ = RETRY_TIL_ZERO_COUNT;		\
	int _resultb_;					\
	while ( _attemptc_-- ){				\
		_resultb_ = (exp);			\
		if (( _resultb_ == 0 )) break;		\
		printk("%s: RETRY_TIL_ZERO = %d times.\n", __FILE__, (RETRY_TIL_ZERO_COUNT - _attemptc_));	\
		msleep(I2C_RW_MDELAY);			\
	}						\
	_resultb_;					\
})

#define REGMAP_RETRY_READ( ds90ub94x, reg, val)		\
({							\
	int _attemptc_ = REGMAP_RETRY_COUNT;		\
	int val_read;					\
	int _resultb_;					\
	while ( _attemptc_-- ){				\
		_resultb_ = 0;				\
		regmap_read(ds90ub94x->regmap, reg, &val_read);	\
		dev_dbg(ds90ub94x->dev, "REGMAP_RETRY_READ: reg 0x%02x, value 0x%02x\n", reg, val_read); \
		if (( val_read == val )) break;		\
		dev_err(ds90ub94x->dev, "REGMAP_RETRY_READ: failed: reg 0x%02x, value 0x%02x, retry = %d\n", reg, val_read, (REGMAP_RETRY_COUNT - _attemptc_)); \
		_resultb_ = -1;				\
		msleep(I2C_RW_MDELAY);			\
	}						\
	_resultb_;					\
})

#define REGMAP_RETRY_WRITE( ds90ub94x, reg, val)	\
({							\
	int _attemptc_ = REGMAP_RETRY_COUNT;		\
	int _resultb_;					\
	while ( _attemptc_-- ){				\
		_resultb_ = 0;				\
		_resultb_ = regmap_write(ds90ub94x->regmap, reg, val);	\
		dev_dbg(ds90ub94x->dev, "REGMAP_RETRY_WRITE: reg 0x%02x, value 0x%02x\n", reg, val);	\
		if (( _resultb_ >= 0 )) break;		\
		dev_err(ds90ub94x->dev, "REGMAP_RETRY_WRITE: failed: reg 0x%02x, value 0x%02x, retry =%d \n", reg, val, (REGMAP_RETRY_COUNT - _attemptc_));	\
		msleep(I2C_RW_MDELAY);			\
	}						\
	_resultb_;					\
})

static const struct regmap_config config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xFF,
};

struct ds90ub94x {
	struct device *dev;
	struct regmap *regmap;
	struct i2c_config *reset;
	struct i2c_config *probe_info;
	struct i2c_config *dual_channel;
	int reset_size;
	int probe_info_size;
	int dual_channel_size;
};

struct i2c_config{
	unsigned char i2c_reg;
	unsigned char i2c_val;
};

struct i2c_config ds90ub941_reset[] = {
	{0x01, 0x0E},
	{0x04, 0x20}, //clear CRC_ERROR count
};

struct i2c_config ds90ub948_reset[] = {
	{0x01, 0x03},
};

struct i2c_config ds90ub941_dual_channel[] = {
	{0x5B, 0x29},
};

struct i2c_config ds90ub948_dual_channel[] = {
	{0x49, 0x60},
};

struct i2c_config ds90ub941_probe_config[] = {
	{0x04, 0x00}, //start CRC_ERROR count
	{0x03, 0x9A}, //Enable FPD-Link I2C pass through, DSI clock auto switch

	{0x1E, 0x01}, //Select FPD-Link III Port 0
	{0x5B, 0x21}, //FPD3_TX_MODE=single, Reset PLL
	{0x4F, 0x8C}, //DSI Continuous Clock Mode,DSI 4 lanes

	{0x01, 0x00}, //Release DSI/DIGITLE reset

	{0x07, 0x54}, //SlaveID_0: touch panel, 0x2A << 1
	{0x08, 0x54}, //SlaveAlias_0: touch panel, 0x2A << 1
	{0x70, 0xAC}, //SlaveID_1: EEPROM, 0x56 << 1
	{0x77, 0xAC}, //SlaveAlias_1: EEPROM, 0x56 << 1

	{0xC6, 0x21}, //REM_INTB the same as INTB_IN on UB948, Global Interrupt Enable 
};

struct i2c_config ds90ub948_probe_config[] = {
	{0x03, 0xF8}, //enable CRC, i2c pass through
	{0x49, 0x62}, //Set FPD_TX_MODE, MAPSEL=1(SPWG), Single OLDI output
	{0x34, 0x02}, //Select FPD-Link III Port 0, GPIOx instead of D_GPIOx
	{0x1D, 0x19}, //GPIO0, MIPI_BL_EN
	{0x1E, 0x99}, //GPIO1, MIPI_VDDEN; GPIO2, MIPI_BL_PWM

	{0x1F, 0x09}, //Reset touch interrupt

	{0x08, 0x54}, //TargetID_0: touch panel, 0x2A << 1
	{0x10, 0x54}, //TargetALIAS_0: touch panel, 0x2A << 1
	{0x09, 0xAC}, //TargetID_1: EEPROM, 0x56 << 1
	{0x11, 0xAC}, //TargetALIAS_1: EEPROM, 0x56 << 1
};

static int regmap_i2c_rw_check_retry(struct ds90ub94x *ds90ub94x, struct i2c_config *i2c_config, int i2c_config_size)
{
	int i;
	unsigned int reg, val;

	//continue write
	for ( i = 0; i < i2c_config_size ; i++ )
	{
		reg = i2c_config[i].i2c_reg;
		val = i2c_config[i].i2c_val;

		if (REGMAP_RETRY_WRITE(ds90ub94x, reg, val) < 0)
			return -EIO;
	}

	//continue read check
	for ( i = 0; i < i2c_config_size ; i++ )
	{
		reg = i2c_config[i].i2c_reg;
		val = i2c_config[i].i2c_val;

		if (REGMAP_RETRY_READ(ds90ub94x, reg, val) != 0)
			return -EIO;
	}
	return 0;
}

static int ds90ub94x_init(struct ds90ub94x *ds90ub94x)
{
	int i;
	unsigned char reg, val;

	// reset reg can't read back the same val
	for ( i = 0 ; i < ds90ub94x->reset_size ; i++ )
	{
		reg = ds90ub94x->reset[i].i2c_reg;
		val = ds90ub94x->reset[i].i2c_val;

		if (REGMAP_RETRY_WRITE(ds90ub94x, reg, val) < 0)
			return -EIO;
	}

	if ( RETRY_TIL_ZERO(regmap_i2c_rw_check_retry(ds90ub94x, ds90ub94x->probe_info, ds90ub94x->probe_info_size)) != 0 ) {
		dev_err(ds90ub94x->dev, "ds90ub94x init fail\n");
		return -EIO;
	}

	return 0;
}

static int ds90ub94x_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct ds90ub94x *ds90ub941, *ds90ub948;
	struct i2c_client *dummy_client;
	struct gpio_desc *reset_gpio;
	int ret_ser, ret_des, i, crc_error_1, crc_error_2;
	int ret = 0;

	ds90ub941 = devm_kzalloc(&client->dev, sizeof(struct ds90ub94x), GFP_KERNEL);
	ds90ub948 = devm_kzalloc(&client->dev, sizeof(struct ds90ub94x), GFP_KERNEL);

	if ( !ds90ub941 || !ds90ub948 ) {
		ret = -ENOMEM;
		goto req_failed;
	}

	i2c_set_clientdata(client, ds90ub941);
	//add new i2c_device for UB948
	dummy_client = i2c_new_dummy_device(client->adapter, DS90UB948_I2C_ADDR);

	ds90ub941->dev = &client->dev;
	ds90ub941->regmap = devm_regmap_init_i2c(client, &config);
	ds90ub941->reset = ds90ub941_reset;
	ds90ub941->probe_info = ds90ub941_probe_config;
	ds90ub941->dual_channel = ds90ub941_dual_channel;
	ds90ub941->reset_size = ARRAY_SIZE(ds90ub941_reset);
	ds90ub941->probe_info_size = ARRAY_SIZE(ds90ub941_probe_config);
	ds90ub941->dual_channel_size = ARRAY_SIZE(ds90ub941_dual_channel);

	ds90ub948->dev = &dummy_client->dev;
	ds90ub948->regmap = devm_regmap_init_i2c(dummy_client, &config);
	ds90ub948->reset = ds90ub948_reset;
	ds90ub948->probe_info = ds90ub948_probe_config;
	ds90ub948->dual_channel = ds90ub948_dual_channel;
	ds90ub948->reset_size = ARRAY_SIZE(ds90ub948_reset);
	ds90ub948->probe_info_size = ARRAY_SIZE(ds90ub948_probe_config);
	ds90ub948->dual_channel_size = ARRAY_SIZE(ds90ub948_dual_channel);

	if (IS_ERR(ds90ub941->regmap)) {
		ret = PTR_ERR(ds90ub941->regmap);
		goto req_failed;
	}
	else if (IS_ERR(ds90ub948->regmap)) {
		ret = PTR_ERR(ds90ub948->regmap);
		goto req_failed;
	}

	reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
					     GPIOD_OUT_HIGH);

	if (! reset_gpio) {
		dev_err(ds90ub941->dev,"reset-gpio get failed\n");
		ret = PTR_ERR(reset_gpio);
		goto req_failed;
	}

	for ( i = 0; i<=RETRY_TIL_ZERO_COUNT; i++) {
		if ( i == RETRY_TIL_ZERO_COUNT ) {
			ret = -EIO;
			goto err_out;
		}

		gpiod_direction_output(reset_gpio, 0);
		msleep(RESET_MDELAY);
		gpiod_direction_output(reset_gpio, 1);
		msleep(RESET_MDELAY);
		gpiod_direction_output(reset_gpio, 0);
		msleep(RESET_MDELAY);

		ret_ser = ds90ub94x_init(ds90ub941);
		msleep(RESET_MDELAY);
		ret_des = ds90ub94x_init(ds90ub948);

		if ( (! ret_ser) && (! ret_des) )
			break;

		if ( ret_ser )
			dev_err(ds90ub941->dev,"=====> init failed, full_retry = %d times\n", i);
		if ( ret_des )
			dev_err(ds90ub948->dev,"=====> init failed, full_retry = %d times\n", i);
	}

	//read attribute to set dual lvds channel
        if (ds90ub941->dev->of_node) {
                struct device_node *dev_node = ds90ub941->dev->of_node;
                if (of_property_read_bool(dev_node, "vizionpanel-dual-lvds-channel")) {
			dev_info(ds90ub941->dev, "Using Dual channel lvds\n");

			if ( RETRY_TIL_ZERO(regmap_i2c_rw_check_retry(ds90ub941, ds90ub941->dual_channel, ds90ub941->dual_channel_size)) < 0 ) {
				ret = -EIO;
				goto err_out;
			}
			msleep(I2C_RW_MDELAY);
			if ( RETRY_TIL_ZERO(regmap_i2c_rw_check_retry(ds90ub948, ds90ub948->dual_channel, ds90ub948->dual_channel_size)) < 0 ) {
				ret = -EIO;
				goto err_out;
			}
                } else
			dev_info(ds90ub941->dev, "Using Single channel lvds\n");
        }

	regmap_read(ds90ub941->regmap, 0x0A, &crc_error_1);
	regmap_read(ds90ub941->regmap, 0x0B, &crc_error_2);
	dev_info(ds90ub941->dev, "ds90ub94x probe success with CRC_ERROR_COUNT = 0x%02x%02x\n", crc_error_2, crc_error_1);
	return ret;

err_out:
	regmap_read(ds90ub941->regmap, 0x0A, &crc_error_1);
	regmap_read(ds90ub941->regmap, 0x0B, &crc_error_2);
	dev_err(ds90ub941->dev, "ds90ub94x probe failed with CRC_ERROR_COUNT = 0x%02x%02x\n", crc_error_2, crc_error_1);
	return ret;

req_failed:
	dev_err(ds90ub941->dev, "request memery/regmap/reset-gpios failed\n");
	return ret;
}

static int ds90ub94x_remove(struct i2c_client *client)
{
	return 0;
}

static const struct of_device_id ds90ub94x_of_match[] = {
	{
		.compatible = "ti,ds90ub94x"
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ds90ub94x_of_match);

static struct i2c_driver ds90ub94x_driver = {
	.driver = {
		.name = "ds90ub94x",
		.of_match_table = ds90ub94x_of_match,
	},
	.probe = ds90ub94x_probe,
	.remove = ds90ub94x_remove,
};
module_i2c_driver(ds90ub94x_driver);

MODULE_DESCRIPTION("TI. DS90UB941/DS90UB948 SerDer bridge");
MODULE_LICENSE("GPL");
