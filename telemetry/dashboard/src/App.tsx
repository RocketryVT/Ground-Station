import { useState } from 'react';
import { Group, Panel, Separator } from 'react-resizable-panels';

import './App.css';
import { useMQTT }          from './hooks/useMQTT';
import { useDemoMode }      from './hooks/useDemoMode';
import { StatusBar }        from './components/StatusBar/StatusBar';
import { TrajectoryMap }    from './components/Map/TrajectoryMap';
import { RocketScene }      from './components/Scene3D/RocketScene';
import { AntennaScene }     from './components/AntennaScene/AntennaScene';
import { TelemetryCharts }  from './components/Charts/TelemetryCharts';
import { VideoFeed }        from './components/VideoFeed/VideoFeed';

function Handle() {
  return (
    <Separator className="handle">
      <div className="handle__bar" />
    </Separator>
  );
}

export default function App() {
  const [demo, setDemo] = useState(false);

  useMQTT(!demo);
  useDemoMode(demo);

  return (
    <div className="root">
      <header className="status-bar">
        <StatusBar demo={demo} onToggleDemo={() => setDemo((d) => !d)} />
      </header>

      <div className="content">
        <Group orientation="horizontal">

          {/* -- Left column: map + 3-D views --------------------------- */}
          <Panel defaultSize={50} minSize={20} className="fill">
            <Group orientation="vertical">
              <Panel defaultSize={50} minSize={15} className="fill">
                <div className="panel"><TrajectoryMap /></div>
              </Panel>
              <Handle />
              {/* Orientation + antenna tracker side by side */}
              <Panel defaultSize={50} minSize={10} className="fill">
                <Group orientation="horizontal">
                  <Panel defaultSize={50} minSize={20} className="fill">
                    <div className="panel"><RocketScene /></div>
                  </Panel>
                  <Handle />
                  <Panel defaultSize={50} minSize={20} className="fill">
                    <div className="panel"><AntennaScene /></div>
                  </Panel>
                </Group>
              </Panel>
            </Group>
          </Panel>

          <Handle />

          {/* -- Right column: video + charts --------------------------- */}
          <Panel defaultSize={50} minSize={20} className="fill">
            <Group orientation="vertical">

              {/* Video feed */}
              <Panel defaultSize={66} minSize={15} className="fill">
                <div className="panel"><VideoFeed /></div>
              </Panel>

              <Handle />

              {/* Live telemetry charts */}
              <Panel defaultSize={40} minSize={12} className="fill">
                <div className="panel"><TelemetryCharts /></div>
              </Panel>

            </Group>
          </Panel>

        </Group>
      </div>
    </div>
  );
}

