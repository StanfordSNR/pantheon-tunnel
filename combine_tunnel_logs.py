#!/usr/bin/python
received_packets = dict()
with open('/tmp/tunnelserver.ingress.log') as received_log:
    firstline = True
    for line in received_log:
        if firstline:
            ( _, _, received_initial_timestamp ) = line.split(':')
            firstline = False
        else:
            (received_timestamp, received_uid, received_size) = line.split('-')
            received_packets[int(received_uid)] = (int(received_timestamp), int(received_size))

client_packets = dict()
unsorted_combined_log = []
with open('/tmp/tunnelclient.egress.log') as sent_log:
    firstline = True
    for line in sent_log:
        if firstline:
            ( _, _, sent_initial_timestamp ) = line.split(':')
            firstline = False
        else:
            (sent_timestamp, sent_uid, sent_size) = line.split('-')
            # drop whitespace
            sent_timestamp = str(int(sent_timestamp))
            sent_uid = str(int(sent_uid))
            sent_size = str(int(sent_size))

            unsorted_combined_log.append( sent_timestamp + ' + ' + str(int(sent_size)) )
            if int(sent_uid) in received_packets:
                (received_timestamp, received_size) = received_packets[int(sent_uid)]
                received_timestamp = int(received_timestamp) - (int(sent_initial_timestamp) - int(received_initial_timestamp))
                if received_size != int(sent_size):
                    print("packet " + sent_uid + " came into tunnel with size " + sent_size + " but left with size " + str(received_size) )
                    assert(False)
                unsorted_combined_log.append( str(received_timestamp) + ' # ' + sent_size )
                unsorted_combined_log.append( str(received_timestamp) + ' - ' + sent_size + ' ' + str( received_timestamp - int(sent_timestamp) ) )

print("# base timestamp: 0" )
#for line in sorted( unsorted_combined_log, cmp=lambda x,y: cmp(int(x.split()[0]), int(y.split()[0])) ):
for line in unsorted_combined_log:
    print(line)
