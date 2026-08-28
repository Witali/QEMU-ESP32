@{
    ideal = @{
        BandwidthBps = 0
        LatencyMs = 0
        JitterMs = 0
        LossPpm = 0
        QueueLimit = 256
        LinkDown = $false
    }
    hardware = @{
        BandwidthBps = 4000000
        LatencyMs = 20
        JitterMs = 5
        LossPpm = 0
        QueueLimit = 64
        LinkDown = $false
    }
    weak = @{
        BandwidthBps = 1000000
        LatencyMs = 80
        JitterMs = 30
        LossPpm = 10000
        QueueLimit = 32
        LinkDown = $false
    }
    congested = @{
        BandwidthBps = 256000
        LatencyMs = 250
        JitterMs = 100
        LossPpm = 30000
        QueueLimit = 8
        LinkDown = $false
    }
    offline = @{
        BandwidthBps = 0
        LatencyMs = 0
        JitterMs = 0
        LossPpm = 0
        QueueLimit = 1
        LinkDown = $true
    }
}
