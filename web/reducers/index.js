import { combineReducers } from 'redux';
import { enableBatching } from 'redux-batched-actions';
import data from './data';
import accelerometer from './accelerometer';
import gyroscope from './gyroscope';
import compass from './compass';
import gravity from './gravity';
import heading from './heading';
import pressure from './pressure';
import humidity from './humidity';
import temperature from './temperature';
import device from './device';
import eco2 from './eco2';
import tvoc from './tvoc';
import batteryLevel from './batteryLevel';

export default enableBatching(
    combineReducers({
        data,
        device,
        accelerometer,
        gyroscope,
        compass,
        gravity,
        heading,
        pressure,
        humidity,
        temperature,
        eco2,
        tvoc,
        batteryLevel,
    }),
);
